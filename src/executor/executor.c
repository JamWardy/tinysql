#include "executor.h"

#include <assert.h>

#include "error.h"
#include "internal.h"

static int execute_statement(Database *db, const Statement *stmt,
                             const ParamList *params, Result *result,
                             Error *error) {
    switch (stmt->type) {
        case STMT_CREATE:
            return execute_create(db, stmt, error);
        case STMT_INSERT:
            return execute_insert(db, stmt, params, result, error);
        case STMT_SELECT:
            return execute_select(db, stmt, params, result, error);
        case STMT_DELETE:
            return execute_delete(db, stmt, params, result, error);
        case STMT_UPDATE:
            return execute_update(db, stmt, params, result, error);
        case STMT_DROP:
            return execute_drop(db, stmt, error);
        case STMT_CREATE_INDEX:
            return execute_create_index(db, stmt, error);
        case STMT_DROP_INDEX:
            return execute_drop_index(db, stmt, error);
        case STMT_ANALYZE:
            return execute_analyze(db, stmt, error);
        case STMT_BEGIN:
        case STMT_COMMIT:
        case STMT_ROLLBACK:
            return 1;  // tinysql_run does these, as they need the file.
    }

    return fail(error, ERROR_INTERNAL, "unknown statement type");
}

int execute(Database *db, const Statement *stmt, const ParamList *params,
            Result *result, Error *error) {
    assert(result->column_count == 0 && result->row_count == 0 &&
           result->changed == 0);

    if ((size_t)stmt->param_count != params->count) {
        return fail(error, ERROR_PROTOCOL_VIOLATION,
                    "statement has %d parameters but %zu values were given",
                    stmt->param_count, params->count);
    }

    int ok = stmt->explain
                 ? execute_explain(db, stmt, params, result, error)
                 : execute_statement(db, stmt, params, result, error);

    // A SELECT can fail partway through filling result, e.g. running out
    // of memory, so empty it here rather than on every error path.
    if (!ok) {
        result_free(result);
        return 0;
    }

    // EXPLAIN's result is rows, like a SELECT's.
    result->type = stmt->explain ? STMT_SELECT : stmt->type;
    return 1;
}
