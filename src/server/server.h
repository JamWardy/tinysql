#ifndef TINYSQL_SERVER_SERVER_H
#define TINYSQL_SERVER_SERVER_H

#include "tinysql.h"

// Serves db, loaded from path, on 127.0.0.1:port until SIGINT or SIGTERM, one
// statement at a time from whichever connection is ready. Returns 0 after a
// clean stop, or 1 if the server couldn't start.
int server_run(Database *db, const char *path, int port);

#endif
