#ifndef TINYSQL_EXECUTOR_H
#define TINYSQL_EXECUTOR_H

#include <stddef.h>

#include "parser.h"
#include "table.h"

// Runs one parsed statement against db; SELECT prints its rows to stdout.
// On success returns 1. On failure writes a message to error and returns
// 0; db is unchanged.
int execute(Database *db, const Statement *stmt,
            char *error, size_t error_size);

#endif
