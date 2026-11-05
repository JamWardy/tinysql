#include "internal.h"

#include <stdio.h>

#include "storage/transaction.h"

// A replica this far behind is dropped, so a stalled one can't grow the
// primary's memory without bound, as Redis's client-output-buffer-limit does.
// It resyncs when it reconnects.
#define MAX_REPLICA_BACKLOG (64 * 1024 * 1024)

static int has_replicas(const Server *server) {
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (server->connections[i].state == CONNECTION_REPLICA) {
            return 1;
        }
    }
    return 0;
}

// Every replica is missing a change it can't now be sent.
static void drop_replicas(Server *server, const char *why) {
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        Connection *conn = &server->connections[i];

        if (conn->state == CONNECTION_REPLICA) {
            fprintf(stderr, "tinysql-server: dropped a replica: %s\n", why);
            close_connection(server, conn);
        }
    }
}

static int read_file(const char *path, Buffer *out, Error *error) {
    unsigned char chunk[65536];
    size_t count;
    FILE *file = fopen(path, "rb");

    if (!file) {
        return fail(error, ERROR_IO, "cannot open '%s'", path);
    }

    buffer_init(out);
    while ((count = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (!buffer_append(out, chunk, count)) {
            fclose(file);
            buffer_free(out);
            return fail(error, ERROR_OUT_OF_MEMORY, "out of memory");
        }
    }

    int failed = ferror(file);
    fclose(file);
    if (failed) {
        buffer_free(out);
        return fail(error, ERROR_IO, "error reading '%s'", path);
    }
    return 1;
}

void serve_replicate(Server *server, Connection *conn) {
    unsigned char type;
    size_t body_length;
    Buffer file;
    Error error;

    (void)protocol_next_frame(&conn->in, PROTOCOL_MAX_BODY, &type,
                              &body_length);
    buffer_consume(&conn->in, PROTOCOL_HEADER_SIZE + body_length);

    // No transaction is open, as 'R' waits like a query, so saving makes the
    // file hold exactly what's committed, including a change whose own save
    // failed.
    if (!transaction_autocommit(server->db, server->path, &error) ||
        !read_file(server->path, &file, &error)) {
        fprintf(stderr, "tinysql-server: cannot send a snapshot: %s\n",
                error.message);
        send_last_error(server, conn, &error);
        return;
    }

    if (!protocol_write_snapshot(&conn->out, &file)) {
        buffer_free(&file);
        close_connection(server, conn);
        return;
    }

    fprintf(stderr, "tinysql-server: a replica connected; sent a %zu-byte "
                    "snapshot\n", file.length);
    buffer_free(&file);
    conn->state = CONNECTION_REPLICA;
}

void record_change(Server *server, const Query *query) {
    PendingUnit *unit = &server->as.unit;

    // A replica can't connect mid-transaction, so with none now, none can
    // need this unit.
    if (!has_replicas(server)) {
        return;
    }

    if (!protocol_append_statement(&unit->statements, query->sql,
                                   query->params, query->param_count)) {
        drop_replicas(server, "out of memory");
        discard_unit(server);
        return;
    }
    unit->count++;
}

void send_unit(Server *server) {
    PendingUnit *unit = &server->as.unit;

    if (unit->count == 0) {
        return;
    }

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        Connection *conn = &server->connections[i];

        if (conn->state != CONNECTION_REPLICA) {
            continue;
        }
        if (!protocol_write_unit(&conn->out, &unit->statements,
                                 unit->count) ||
            conn->out.length > MAX_REPLICA_BACKLOG) {
            fprintf(stderr, "tinysql-server: dropped a replica: too far "
                            "behind\n");
            close_connection(server, conn);
        }
    }
    discard_unit(server);
}

void discard_unit(Server *server) {
    buffer_free(&server->as.unit.statements);
    server->as.unit.count = 0;
}
