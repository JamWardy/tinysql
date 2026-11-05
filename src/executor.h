#ifndef TINYSQL_EXECUTOR_H
#define TINYSQL_EXECUTOR_H

#include <stddef.h>

#include "parser.h"
#include "result.h"
#include "storage/table.h"

// One per `?`, in order. Text is read, never kept.
typedef struct {
    const Value *values;  // May be NULL when count is 0.
    size_t count;
} ParamList;

// result must be empty, as result_init leaves it. An EXPLAIN fills it with the
// plans instead, and changes nothing. On failure result is left empty, but an
// UPDATE that ran out of memory may have changed some rows.
int execute(Database *db, const Statement *stmt, const ParamList *params,
            Result *result, Error *error);

#endif
