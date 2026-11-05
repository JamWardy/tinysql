#ifndef TINYSQL_SERVER_INTERNAL_H
#define TINYSQL_SERVER_INTERNAL_H

// Shared by the server's files and included nowhere else; main.c includes
// server.h.

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
    CONNECTION_WAITING,  // Has a query to run, perhaps behind a transaction.
    CONNECTION_WRITING,  // Sending the reply.
    CONNECTION_CLOSING,  // Sending a last error, then closing.
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

typedef struct {
    Database *db;
    const char *path;
    int listener;
    Connection connections[MAX_CONNECTIONS];
    Connection *owner;  // The one whose transaction is open, or NULL.
    unsigned long long next_ticket;
} Server;

#endif
