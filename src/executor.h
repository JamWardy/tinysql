#ifndef TINYSQL_EXECUTOR_H
#define TINYSQL_EXECUTOR_H

#include <stddef.h>

#include "parser.h"
#include "result.h"
#include "table.h"

// Runs one parsed statement against db and fills result, which must be
// empty, as result_init and result_free leave it: a SELECT's columns and
// rows, or how many rows an INSERT, UPDATE or DELETE changed. The caller
// frees result with result_free.
// On success returns 1. On failure writes a message to error and returns
// 0; db is unchanged and result is left empty.
int execute(Database *db, const Statement *stmt, Result *result,
            char *error, size_t error_size);

#endif
