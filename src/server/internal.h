#ifndef TINYSQL_SERVER_INTERNAL_H
#define TINYSQL_SERVER_INTERNAL_H

// Shared by the server's files and included nowhere else; main.c includes
// server.h.

#include <stddef.h>
#include <sys/select.h>

#include "protocol.h"
#include "server.h"

// select can't watch a descriptor at or above FD_SETSIZE (often 1024), so
// that's the real limit; this leaves room for the listener and stdio.
#define MAX_CONNECTIONS 1000

// A transaction idle this long is rolled back, so a stalled client can't hold
// every other one up, as Postgres's idle_in_transaction_session_timeout does.
#define IDLE_IN_TRANSACTION_TIMEOUT 5.0  // Seconds.

typedef enum {
    CONNECTION_UNUSED,   // A free slot.
    CONNECTION_READING,  // Gathering a whole message.
    CONNECTION_WAITING,  // Has a 'Q' or 'R' to serve, perhaps behind a
                         // transaction.
    CONNECTION_WRITING,  // Sending the reply.
    CONNECTION_CLOSING,  // Sending a last error, then closing.
    CONNECTION_REPLICA,  // A replica of this server, sent each commit.
} ConnectionState;

typedef struct {
    ConnectionState state;
    int fd;
    Buffer in;
    Buffer out;
    unsigned long long ticket;  // CONNECTION_WAITING's place in the queue.
    double last_reply;          // When its last reply was ready.
    int timed_out;              // Its transaction was rolled back for idling.
} Connection;

// What the open transaction has changed so far. Replicas get it as one unit
// once no transaction is open, or never, if it's rolled back.
typedef struct {
    Buffer statements;  // As protocol_append_statement adds them.
    size_t count;
} PendingUnit;

typedef enum {
    UPSTREAM_DISCONNECTED,  // Connects again at retry_at.
    UPSTREAM_CONNECTING,    // Waiting for connect() to finish.
    UPSTREAM_SYNCING,       // Sent 'R'; waiting for the snapshot.
    UPSTREAM_STREAMING,     // Applying units as they come.
} UpstreamState;

// A replica's connection to its primary.
typedef struct {
    UpstreamState state;
    int primary_port;
    int fd;           // Unless UPSTREAM_DISCONNECTED.
    Buffer in;
    Buffer out;       // The 'R', until it's sent.
    double retry_at;  // UPSTREAM_DISCONNECTED's.
} Upstream;

typedef enum {
    SERVER_PRIMARY,
    SERVER_REPLICA,  // Copies a primary, and is read-only.
} ServerRole;

typedef struct {
    Database *db;
    const char *path;
    int listener;
    Connection connections[MAX_CONNECTIONS];
    Connection *owner;  // The one whose transaction is open, or NULL.
    unsigned long long next_ticket;
    ServerRole role;
    union {
        PendingUnit unit;   // SERVER_PRIMARY's, for its replicas.
        Upstream upstream;  // SERVER_REPLICA's, to its primary.
    } as;
} Server;

// server.c

double now_seconds(void);
int set_non_blocking(int fd);
void close_connection(Server *server, Connection *conn);

// Sends error, then closes the connection.
void send_last_error(Server *server, Connection *conn, const Error *error);

// Runs a statement of the server's own, outside any client's turn.
int run_statement(Server *server, const char *sql, const Value *params,
                  size_t param_count, Error *error);

// Rolls back the open transaction, which no one owns once it's done.
void rollback_owner(Server *server, const char *why);

// primary.c: sending what's committed to replicas. Only for SERVER_PRIMARY.

// Answers a waiting 'R' with the snapshot, and makes conn a replica.
void serve_replicate(Server *server, Connection *conn);

// Adds a statement that changed the database to the unit being gathered.
void record_change(Server *server, const Query *query);

// Sends the unit to every replica, once no transaction is open.
void send_unit(Server *server);

// The unit's transaction was rolled back, so nothing in it happened.
void discard_unit(Server *server);

// replica.c: copying a primary. Only for SERVER_REPLICA.

void upstream_init(Upstream *upstream, int primary_port);
void upstream_close(Upstream *upstream);
void upstream_connect_if_due(Server *server);

// Adds the upstream's descriptor to what select watches.
void upstream_watch(const Upstream *upstream, fd_set *reads, fd_set *writes,
                    int *highest);
void upstream_handle_ready(Server *server, const fd_set *reads,
                           const fd_set *writes);

// Applies what's arrived from the primary, while no transaction is open.
void upstream_apply(Server *server);

// When to try connecting again. Fails if it isn't waiting to.
int upstream_retry_at(const Upstream *upstream, double *at);

#endif
