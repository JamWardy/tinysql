// sigaction, pselect, clock_gettime and the socket calls are POSIX, not C11,
// so glibc hides them under -std=c11 unless asked.
#define _DEFAULT_SOURCE

#include "internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static double now_seconds(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

// The database can't carry on with a transaction nobody can end, and the file
// still holds every commit, so stopping loses nothing.
static void fail_rollback(const Error *error) {
    fprintf(stderr, "tinysql-server: cannot roll back, stopping: %s\n",
            error->message);
    exit(1);
}

static void rollback_owner(Server *server, const char *why) {
    Result result;
    Error error;

    result_init(&result);
    if (!tinysql_run(server->db, server->path, "ROLLBACK;", NULL, 0,
                     &result, &error)) {
        fail_rollback(&error);
    }
    result_free(&result);

    fprintf(stderr, "tinysql-server: rolled back a transaction: %s\n", why);
    server->owner = NULL;
}

// ---- Connections ----

static void close_connection(Server *server, Connection *conn) {
    if (server->owner == conn) {
        rollback_owner(server, "its connection closed");
    }

    close(conn->fd);
    buffer_free(&conn->in);
    buffer_free(&conn->out);
    conn->state = CONNECTION_UNUSED;
}

// Out of memory leaves no way to answer, so the connection is closed.
static void send_last_error(Server *server, Connection *conn,
                            ErrorCode code, const char *message) {
    Error error;

    fail(&error, code, "%s", message);
    if (!protocol_write_error(&conn->out, &error)) {
        close_connection(server, conn);
        return;
    }
    conn->state = CONNECTION_CLOSING;
}

// Looks at the first message in conn->in, once conn is ready for one.
static void take_message(Server *server, Connection *conn) {
    unsigned char type;
    size_t body_length;

    switch (protocol_next_frame(&conn->in, &type, &body_length)) {
        case FRAME_INCOMPLETE:
            return;
        case FRAME_TOO_BIG:
            send_last_error(server, conn, ERROR_PROTOCOL_VIOLATION,
                            "message is too big");
            return;
        case FRAME_READY:
            break;
    }

    if (type == MESSAGE_QUERY) {
        conn->state = CONNECTION_WAITING;
        conn->ticket = server->next_ticket++;
    } else if (type == MESSAGE_TERMINATE) {
        close_connection(server, conn);
    } else {
        send_last_error(server, conn, ERROR_PROTOCOL_VIOLATION,
                        "unknown message type");
    }
}

static int set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static Connection *free_slot(Server *server) {
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (server->connections[i].state == CONNECTION_UNUSED) {
            return &server->connections[i];
        }
    }
    return NULL;
}

// Several clients may be waiting in the backlog, so accepts until none are.
static void accept_connections(Server *server) {
    while (1) {
        int fd = accept(server->listener, NULL, NULL);

        if (fd < 0) {
            return;  // EAGAIN: no more for now; anything else, try later.
        }

        Connection *conn = free_slot(server);
        if (!conn || fd >= FD_SETSIZE || !set_non_blocking(fd)) {
            fprintf(stderr, "tinysql-server: refused a connection: "
                            "too many open\n");
            close(fd);
            continue;
        }

        conn->state = CONNECTION_READING;
        conn->fd = fd;
        buffer_init(&conn->in);
        buffer_init(&conn->out);
        conn->timed_out = 0;
    }
}

static void receive(Server *server, Connection *conn) {
    unsigned char chunk[65536];
    ssize_t count = recv(conn->fd, chunk, sizeof(chunk), 0);

    if (count == 0) {
        close_connection(server, conn);  // The client hung up.
        return;
    }
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close_connection(server, conn);
        }
        return;
    }
    if (!buffer_append(&conn->in, chunk, (size_t)count)) {
        close_connection(server, conn);
        return;
    }
    take_message(server, conn);
}

// Sends what the socket takes now; the rest goes when select says it's ready.
static void send_reply(Server *server, Connection *conn) {
    ssize_t count = send(conn->fd, conn->out.data, conn->out.length, 0);

    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close_connection(server, conn);
        }
        return;
    }
    buffer_consume(&conn->out, (size_t)count);
    if (conn->out.length > 0) {
        return;
    }

    if (conn->state == CONNECTION_CLOSING) {
        close_connection(server, conn);
        return;
    }

    // The client may have sent its next query already.
    conn->state = CONNECTION_READING;
    take_message(server, conn);
}

// ---- Running queries ----

// Fills reply with T, D..., C, or E, for a 'Q' message's body.
static int run_query(Server *server, Connection *conn,
                     const unsigned char *body, size_t body_length,
                     Buffer *reply) {
    Query query;
    Result result;
    Error error;

    // The timeout's rollback is news to the client, which may be about to
    // COMMIT, so its next query is answered with it instead of being run.
    if (conn->timed_out) {
        conn->timed_out = 0;
        fail(&error, ERROR_IDLE_IN_TRANSACTION_TIMEOUT,
             "transaction was rolled back after %.0f seconds idle",
             IDLE_IN_TRANSACTION_TIMEOUT);
        return protocol_write_error(reply, &error);
    }

    if (!protocol_read_query(body, body_length, &query, &error)) {
        return protocol_write_error(reply, &error);
    }

    result_init(&result);
    int ok = tinysql_run(server->db, server->path, query.sql, query.params,
                         query.param_count, &result, &error);
    query_free(&query);

    int written = ok ? protocol_write_result(reply, &result)
                     : protocol_write_error(reply, &error);
    result_free(&result);
    return written;
}

// A waiting query's whole turn: runs it and queues the reply.
static void serve_query(Server *server, Connection *conn) {
    unsigned char type;
    size_t body_length;
    Buffer reply;

    // take_message saw the whole message, so it's FRAME_READY.
    (void)protocol_next_frame(&conn->in, &type, &body_length);
    buffer_init(&reply);

    int written = run_query(server, conn,
                            conn->in.data + PROTOCOL_HEADER_SIZE,
                            body_length, &reply);
    buffer_consume(&conn->in, PROTOCOL_HEADER_SIZE + body_length);

    // Whoever leaves a transaction open owns it until it ends.
    server->owner = server->db->in_transaction ? conn : NULL;

    if (!written ||
        !protocol_write_ready(&reply, server->db->in_transaction) ||
        !buffer_append(&conn->out, reply.data, reply.length)) {
        buffer_free(&reply);
        close_connection(server, conn);
        return;
    }
    buffer_free(&reply);

    conn->last_reply = now_seconds();
    conn->state = CONNECTION_WRITING;
}

// The longest-waiting query that may run: any, with no transaction open, or
// else only the owner's.
static Connection *next_to_serve(Server *server) {
    Connection *next = NULL;

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        Connection *conn = &server->connections[i];

        if (conn->state != CONNECTION_WAITING ||
            (server->owner && server->owner != conn)) {
            continue;
        }
        if (!next || conn->ticket < next->ticket) {
            next = conn;
        }
    }
    return next;
}

static void serve_waiting_queries(Server *server) {
    Connection *conn;

    while ((conn = next_to_serve(server)) != NULL) {
        serve_query(server, conn);
    }
}

// The owner is idle unless its next query is waiting to run.
static double idle_deadline(const Server *server) {
    return server->owner->last_reply + IDLE_IN_TRANSACTION_TIMEOUT;
}

static void expire_idle_transaction(Server *server) {
    Connection *owner = server->owner;

    if (owner && owner->state != CONNECTION_WAITING &&
        now_seconds() >= idle_deadline(server)) {
        rollback_owner(server, "idle too long");
        owner->timed_out = 1;
    }
}

// ---- The loop ----

static int open_listener(int port) {
    struct sockaddr_in address;
    int yes = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("tinysql-server: socket");
        return -1;
    }

    // Lets a restarted server bind while old connections are in TIME_WAIT.
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, SOMAXCONN) != 0 || !set_non_blocking(fd)) {
        fprintf(stderr, "tinysql-server: cannot listen on 127.0.0.1:%d: %s\n",
                port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// Fills the sets select watches; returns the highest descriptor.
static int watch(const Server *server, fd_set *reads, fd_set *writes) {
    int highest = server->listener;

    FD_ZERO(reads);
    FD_ZERO(writes);
    FD_SET(server->listener, reads);

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        const Connection *conn = &server->connections[i];

        switch (conn->state) {
            case CONNECTION_UNUSED:
            case CONNECTION_WAITING:
                continue;
            case CONNECTION_READING:
                FD_SET(conn->fd, reads);
                break;
            case CONNECTION_WRITING:
            case CONNECTION_CLOSING:
                FD_SET(conn->fd, writes);
                break;
        }
        if (conn->fd > highest) {
            highest = conn->fd;
        }
    }
    return highest;
}

// Until the owner's transaction would time out, or no limit if there's none.
static struct timespec *select_timeout(const Server *server,
                                       struct timespec *timeout) {
    if (!server->owner) {
        return NULL;
    }

    double wait = idle_deadline(server) - now_seconds();
    if (wait < 0) {
        wait = 0;
    }
    timeout->tv_sec = (time_t)wait;
    timeout->tv_nsec = (long)((wait - (double)timeout->tv_sec) * 1e9);
    return timeout;
}

static void handle_ready(Server *server, const fd_set *reads,
                         const fd_set *writes) {
    if (FD_ISSET(server->listener, reads)) {
        accept_connections(server);
    }

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        Connection *conn = &server->connections[i];

        switch (conn->state) {
            case CONNECTION_UNUSED:
            case CONNECTION_WAITING:
                break;
            case CONNECTION_READING:
                if (FD_ISSET(conn->fd, reads)) {
                    receive(server, conn);
                }
                break;
            case CONNECTION_WRITING:
            case CONNECTION_CLOSING:
                if (FD_ISSET(conn->fd, writes)) {
                    send_reply(server, conn);
                }
                break;
        }
    }
}

// A signal is let in only while pselect waits, so one that arrives between
// checking stop_requested and waiting can't be missed: it ends the wait.
static int catch_stop_signals(sigset_t *while_waiting) {
    struct sigaction action;
    sigset_t blocked;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);

    // A client that hangs up mid-reply would otherwise kill the server.
    signal(SIGPIPE, SIG_IGN);

    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0 &&
           sigprocmask(SIG_BLOCK, &blocked, while_waiting) == 0;
}

static void shut_down(Server *server) {
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (server->connections[i].state != CONNECTION_UNUSED) {
            close_connection(server, &server->connections[i]);
        }
    }
    close(server->listener);
}

int server_run(Database *db, const char *path, int port) {
    static Server server;  // Large, so off the stack; there's only one.
    sigset_t while_waiting;

    server.db = db;
    server.path = path;
    server.owner = NULL;
    server.next_ticket = 0;

    if (!catch_stop_signals(&while_waiting)) {
        perror("tinysql-server: signals");
        return 1;
    }

    server.listener = open_listener(port);
    if (server.listener < 0) {
        return 1;
    }
    fprintf(stderr, "tinysql-server: serving '%s' on 127.0.0.1:%d\n",
            path, port);

    while (!stop_requested) {
        fd_set reads;
        fd_set writes;
        struct timespec timeout;
        int highest = watch(&server, &reads, &writes);

        if (pselect(highest + 1, &reads, &writes, NULL,
                    select_timeout(&server, &timeout), &while_waiting) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("tinysql-server: select");
            break;
        }

        handle_ready(&server, &reads, &writes);
        expire_idle_transaction(&server);
        serve_waiting_queries(&server);
    }

    fprintf(stderr, "tinysql-server: stopping\n");
    shut_down(&server);
    return 0;
}
