#include "executor.h"

#include <assert.h>

#include "error.h"
#include "internal.h"

// Runs stmt, filling result, which execute has checked is empty.
static int execute_statement(Database *db, const Statement *stmt,
                             const ParamList *params, Result *result,
                             char *error, size_t error_size) {
    switch (stmt->type) {
        case STMT_CREATE:
            return execute_create(db, stmt, error, error_size);
        case STMT_INSERT:
            return execute_insert(db, stmt, params, result,
                                  error, error_size);
        case STMT_SELECT:
            return execute_select(db, stmt, params, result,
                                  error, error_size);
        case STMT_DELETE:
            return execute_delete(db, stmt, params, result,
                                  error, error_size);
        case STMT_UPDATE:
            return execute_update(db, stmt, params, result,
                                  error, error_size);
        case STMT_DROP:
            return execute_drop(db, stmt, error, error_size);
    }

    return fail(error, error_size, "unknown statement type");
}

int execute(Database *db, const Statement *stmt, const ParamList *params,
            Result *result, char *error, size_t error_size) {
    assert(result->column_count == 0 && result->row_count == 0 &&
           result->changed == 0);

    if ((size_t)stmt->param_count != params->count) {
        return fail(error, error_size,
                    "statement has %d parameters but %zu values were given",
                    stmt->param_count, params->count);
    }

    // A SELECT can fail partway through filling result, e.g. running out
    // of memory, so empty it here rather than on every error path.
    if (!execute_statement(db, stmt, params, result, error, error_size)) {
        result_free(result);
        return 0;
    }

    result->type = stmt->type;
    return 1;
}
