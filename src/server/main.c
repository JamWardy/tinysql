#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "tinysql.h"

#define DEFAULT_PORT 5480

static int usage(const char *program) {
    fprintf(stderr, "Usage: %s <database-file> [--port N]\n", program);
    return 1;
}

static int parse_port(const char *text, int *port) {
    char *end;
    long n = strtol(text, &end, 10);

    if (*text == '\0' || *end != '\0' || n < 1 || n > 65535) {
        return 0;
    }
    *port = (int)n;
    return 1;
}

int main(int argc, char **argv) {
    const char *path;
    int port = DEFAULT_PORT;
    DatabaseLock lock;
    Database db;
    Error error;

    if (argc == 2) {
        path = argv[1];
    } else if (argc == 4 && strcmp(argv[2], "--port") == 0) {
        path = argv[1];
        if (!parse_port(argv[3], &port)) {
            fprintf(stderr, "Error: bad port '%s'\n", argv[3]);
            return 1;
        }
    } else {
        return usage(argv[0]);
    }

    if (!database_lock(path, &lock, &error)) {
        fprintf(stderr, "Error: %s\n", error.message);
        return 1;
    }

    database_init(&db);
    if (!database_load(&db, path, &error)) {
        fprintf(stderr, "Error: cannot load '%s': %s\n", path,
                error.message);
        database_unlock(&lock);
        return 1;
    }

    int status = server_run(&db, path, port);

    database_free(&db);
    database_unlock(&lock);
    return status;
}
