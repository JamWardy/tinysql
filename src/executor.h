#ifndef TINYSQL_EXECUTOR_H
#define TINYSQL_EXECUTOR_H

#include <stddef.h>

#include "parser.h"
#include "result.h"
#include "table.h"

// The values for a statement's `?`s, in order. Text is read, never kept.
typedef struct {
    const Value *values;  // May be NULL when count is 0.
    size_t count;
} ParamList;

// Runs stmt against db, filling result, which must be empty (as
// result_init leaves it); the caller frees it with result_free.
// Returns 1, or 0 with a message in error and result left empty. A
// failed UPDATE may have changed some rows if memory ran out.
int execute(Database *db, const Statement *stmt, const ParamList *params,
            Result *result, char *error, size_t error_size);

#endif
