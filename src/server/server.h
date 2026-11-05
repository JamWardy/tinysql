#ifndef TINYSQL_SERVER_SERVER_H
#define TINYSQL_SERVER_SERVER_H

#include "tinysql.h"

typedef struct {
    int primary_port;
} ReplicaOptions;

typedef struct {
    int port;
    // NULL on a primary. A replica copies its primary and is read-only.
    const ReplicaOptions *replica;
} ServerOptions;

// Serves db, loaded from path, on 127.0.0.1 until SIGINT or SIGTERM, one
// statement at a time from whichever connection is ready. Returns 0 after a
// clean stop, or 1 if the server couldn't start.
int server_run(Database *db, const char *path, const ServerOptions *options);

#endif
