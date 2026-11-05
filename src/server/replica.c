// The socket calls are POSIX, not C11, so glibc hides them under -std=c11
// unless asked.
#define _DEFAULT_SOURCE

#include "internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RETRY_DELAY 1.0  // Seconds.

void upstream_init(Upstream *upstream, int primary_port) {
    upstream->state = UPSTREAM_DISCONNECTED;
    upstream->primary_port = primary_port;
    upstream->retry_at = 0;  // Straight away.
    buffer_init(&upstream->in);
    buffer_init(&upstream->out);
}

void upstream_close(Upstream *upstream) {
    if (upstream->state != UPSTREAM_DISCONNECTED) {
        close(upstream->fd);
    }
    buffer_free(&upstream->in);
    buffer_free(&upstream->out);
    upstream->state = UPSTREAM_DISCONNECTED;
}

// Keeps serving what it has, and tries again soon, as search would rather
// have old results than none.
static void disconnect(Server *server, const char *why) {
    Upstream *upstream = &server->as.upstream;

    // Failing to connect isn't news while the primary is down, so only
    // losing a connection that worked is reported.
    if (upstream->state == UPSTREAM_SYNCING ||
        upstream->state == UPSTREAM_STREAMING) {
        fprintf(stderr, "tinysql-server: lost the primary: %s\n", why);
    }
    upstream_close(upstream);
    upstream->retry_at = now_seconds() + RETRY_DELAY;
}

static void connected(Server *server) {
    Upstream *upstream = &server->as.upstream;

    upstream->state = UPSTREAM_SYNCING;
    if (!protocol_write_replicate(&upstream->out)) {
        disconnect(server, "out of memory");
    }
}

void upstream_connect_if_due(Server *server) {
    Upstream *upstream = &server->as.upstream;
    struct sockaddr_in address;

    if (upstream->state != UPSTREAM_DISCONNECTED ||
        now_seconds() < upstream->retry_at) {
        return;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || fd >= FD_SETSIZE || !set_non_blocking(fd)) {
        if (fd >= 0) {
            close(fd);
        }
        upstream->retry_at = now_seconds() + RETRY_DELAY;
        return;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)upstream->primary_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    upstream->fd = fd;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        connected(server);
    } else if (errno == EINPROGRESS) {
        // A non-blocking connect finishes later; select says when.
        upstream->state = UPSTREAM_CONNECTING;
    } else {
        upstream->state = UPSTREAM_CONNECTING;  // So disconnect closes fd.
        disconnect(server, strerror(errno));
    }
}

void upstream_watch(const Upstream *upstream, fd_set *reads, fd_set *writes,
                    int *highest) {
    switch (upstream->state) {
        case UPSTREAM_DISCONNECTED:
            return;
        case UPSTREAM_CONNECTING:
            FD_SET(upstream->fd, writes);
            break;
        case UPSTREAM_SYNCING:
        case UPSTREAM_STREAMING:
            FD_SET(upstream->fd, reads);
            if (upstream->out.length > 0) {
                FD_SET(upstream->fd, writes);
            }
            break;
    }
    if (upstream->fd > *highest) {
        *highest = upstream->fd;
    }
}

// The connect finished; SO_ERROR says whether it worked.
static void finish_connect(Server *server) {
    int failure = 0;
    socklen_t size = sizeof(failure);

    if (getsockopt(server->as.upstream.fd, SOL_SOCKET, SO_ERROR, &failure,
                   &size) != 0 || failure != 0) {
        disconnect(server, strerror(failure));
        return;
    }
    connected(server);
}

static void send_request(Server *server) {
    Upstream *upstream = &server->as.upstream;
    ssize_t count = send(upstream->fd, upstream->out.data,
                         upstream->out.length, 0);

    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            disconnect(server, strerror(errno));
        }
        return;
    }
    buffer_consume(&upstream->out, (size_t)count);
}

static void receive_from_primary(Server *server) {
    Upstream *upstream = &server->as.upstream;
    unsigned char chunk[65536];
    ssize_t count = recv(upstream->fd, chunk, sizeof(chunk), 0);

    if (count == 0) {
        disconnect(server, "it closed the connection");
        return;
    }
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            disconnect(server, strerror(errno));
        }
        return;
    }
    if (!buffer_append(&upstream->in, chunk, (size_t)count)) {
        disconnect(server, "out of memory");
    }
}

void upstream_handle_ready(Server *server, const fd_set *reads,
                           const fd_set *writes) {
    Upstream *upstream = &server->as.upstream;

    switch (upstream->state) {
        case UPSTREAM_DISCONNECTED:
            return;
        case UPSTREAM_CONNECTING:
            if (FD_ISSET(upstream->fd, writes)) {
                finish_connect(server);
            }
            return;
        case UPSTREAM_SYNCING:
        case UPSTREAM_STREAMING:
            break;
    }

    if (upstream->out.length > 0 && FD_ISSET(upstream->fd, writes)) {
        send_request(server);
    }
    if (upstream->state != UPSTREAM_DISCONNECTED &&
        FD_ISSET(upstream->fd, reads)) {
        receive_from_primary(server);
    }
}

int upstream_retry_at(const Upstream *upstream, double *at) {
    if (upstream->state != UPSTREAM_DISCONNECTED) {
        return 0;
    }
    *at = upstream->retry_at;
    return 1;
}

// ---- Applying ----

static int write_file(const char *path, const unsigned char *bytes,
                      size_t length, Error *error) {
    FILE *file = fopen(path, "wb");

    if (!file) {
        return fail(error, ERROR_IO, "cannot create '%s': %s", path,
                    strerror(errno));
    }

    size_t written = fwrite(bytes, 1, length, file);
    int failed = written != length || ferror(file);
    failed |= fclose(file) != 0;

    if (failed) {
        remove(path);
        return fail(error, ERROR_IO, "error writing '%s'", path);
    }
    return 1;
}

// path with suffix on the end, or NULL if memory ran out.
static char *with_suffix(const char *path, const char *suffix) {
    size_t length = strlen(path);
    size_t suffix_size = strlen(suffix) + 1;
    char *joined = malloc(length + suffix_size);

    if (joined) {
        memcpy(joined, path, length);
        memcpy(joined + length, suffix, suffix_size);
    }
    return joined;
}

// The snapshot becomes this server's file and database. It's loaded before
// it replaces the file, so a bad one leaves both as they were.
static int load_snapshot(Server *server, const unsigned char *bytes,
                         size_t length, Error *error) {
    char *temp_path = with_suffix(server->path, ".snapshot");
    Database fresh;

    if (!temp_path) {
        return fail(error, ERROR_OUT_OF_MEMORY, "out of memory");
    }
    if (!write_file(temp_path, bytes, length, error)) {
        free(temp_path);
        return 0;
    }

    database_init(&fresh);
    if (!database_load(&fresh, temp_path, error)) {
        remove(temp_path);
        free(temp_path);
        return 0;
    }

    if (rename(temp_path, server->path) != 0) {
        fail(error, ERROR_IO, "cannot replace '%s': %s", server->path,
             strerror(errno));
        database_free(&fresh);
        remove(temp_path);
        free(temp_path);
        return 0;
    }
    free(temp_path);

    database_free(server->db);
    *server->db = fresh;  // database_init left no transaction open.
    fprintf(stderr, "tinysql-server: copied the primary's %zu-byte "
                    "snapshot\n", length);
    return 1;
}

// One commit on the primary, as one transaction here. Any failure means the
// two have drifted apart, so the caller resyncs.
static int apply_unit(Server *server, const unsigned char *body,
                      size_t length, Error *error) {
    Unit unit;

    if (!protocol_read_unit(body, length, &unit, error)) {
        return 0;
    }

    int ok = run_statement(server, "BEGIN;", NULL, 0, error);
    for (size_t i = 0; ok && i < unit.count; i++) {
        const Query *statement = &unit.statements[i];

        ok = run_statement(server, statement->sql, statement->params,
                           statement->param_count, error);
    }
    ok = ok && run_statement(server, "COMMIT;", NULL, 0, error);
    unit_free(&unit);

    if (!ok && server->db->in_transaction) {
        rollback_owner(server, "a change from the primary failed");
    }
    return ok;
}

// The primary answers 'R' with an error when it can't make a snapshot.
static int fail_from_primary(const unsigned char *body, size_t length,
                             Error *error) {
    const size_t message_start = 9;  // SQLSTATE, then the message's length.

    if (length < message_start) {
        return fail(error, ERROR_PROTOCOL_VIOLATION,
                    "the primary sent a bad error");
    }
    return fail(error, ERROR_IO, "the primary refused: %.*s",
                (int)(length - message_start), body + message_start);
}

static int apply_message(Server *server, unsigned char type,
                         const unsigned char *body, size_t length,
                         Error *error) {
    Upstream *upstream = &server->as.upstream;

    if (type == MESSAGE_SNAPSHOT && upstream->state == UPSTREAM_SYNCING) {
        if (!load_snapshot(server, body, length, error)) {
            return 0;
        }
        upstream->state = UPSTREAM_STREAMING;
        return 1;
    }
    if (type == MESSAGE_UNIT && upstream->state == UPSTREAM_STREAMING) {
        return apply_unit(server, body, length, error);
    }
    if (type == MESSAGE_ERROR) {
        return fail_from_primary(body, length, error);
    }
    return fail(error, ERROR_PROTOCOL_VIOLATION,
                "unexpected message '%c' from the primary", type);
}

void upstream_apply(Server *server) {
    Upstream *upstream = &server->as.upstream;

    while (!server->owner &&
           (upstream->state == UPSTREAM_SYNCING ||
            upstream->state == UPSTREAM_STREAMING)) {
        unsigned char type;
        size_t length;
        Error error;

        switch (protocol_next_frame(&upstream->in, PROTOCOL_MAX_FROM_PRIMARY,
                                    &type, &length)) {
            case FRAME_INCOMPLETE:
                return;
            case FRAME_TOO_BIG:
                disconnect(server, "message is too big");
                return;
            case FRAME_READY:
                break;
        }

        if (!apply_message(server, type,
                           upstream->in.data + PROTOCOL_HEADER_SIZE, length,
                           &error)) {
            disconnect(server, error.message);
            return;
        }
        buffer_consume(&upstream->in, PROTOCOL_HEADER_SIZE + length);
    }
}
