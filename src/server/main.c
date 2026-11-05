#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "tinysql.h"

#define DEFAULT_PORT 5480

static int usage(const char *program) {
    fprintf(stderr, "Usage: %s <database-file> [--port N] "
                    "[--replica-of PORT]\n", program);
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

// The options after the file, in any order. A replica's options go in
// replica, which options then points to.
static int parse_options(int argc, char **argv, ServerOptions *options,
                         ReplicaOptions *replica) {
    options->port = DEFAULT_PORT;
    options->replica = NULL;

    for (int i = 2; i < argc; i += 2) {
        int *port;

        if (strcmp(argv[i], "--port") == 0) {
            port = &options->port;
        } else if (strcmp(argv[i], "--replica-of") == 0) {
            port = &replica->primary_port;
            options->replica = replica;
        } else {
            usage(argv[0]);
            return 0;
        }

        if (i + 1 >= argc) {
            usage(argv[0]);
            return 0;
        }
        if (!parse_port(argv[i + 1], port)) {
            fprintf(stderr, "Error: bad port '%s'\n", argv[i + 1]);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *path;
    ServerOptions options;
    ReplicaOptions replica;
    DatabaseLock lock;
    Database db;
    Error error;

    if (argc < 2) {
        return usage(argv[0]);
    }
    path = argv[1];
    if (!parse_options(argc, argv, &options, &replica)) {
        return 1;
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

    int status = server_run(&db, path, &options);

    database_free(&db);
    database_unlock(&lock);
    return status;
}
