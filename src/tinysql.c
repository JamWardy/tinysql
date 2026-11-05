#include "tinysql.h"

#include "error.h"
#include "executor.h"
#include "parser.h"
#include "tokeniser.h"
#include "storage/transaction.h"

// Does what a statement needs from the file once it has run: BEGIN,
// COMMIT and ROLLBACK do their work here, as the executor never sees
// the file, and any other change is autocommitted.
static int update_file(Database *db, const char *path, StatementType type,
                       Error *error) {
    switch (type) {
        case STMT_BEGIN:
            return transaction_begin(db, path, error);
        case STMT_COMMIT:
            return transaction_commit(db, path, error);
        case STMT_ROLLBACK:
            return transaction_rollback(db, path, error);
        case STMT_SELECT:
            return 1;
        case STMT_CREATE:
        case STMT_INSERT:
        case STMT_DELETE:
        case STMT_UPDATE:
        case STMT_DROP:
        case STMT_CREATE_INDEX:
        case STMT_DROP_INDEX:
        case STMT_ANALYZE:
            return transaction_autocommit(db, path, error);
    }

    return fail(error, ERROR_INTERNAL, "unknown statement type");
}

int tinysql_run(Database *db, const char *path, const char *sql,
                const Value *params, size_t param_count,
                Result *result, Error *error) {
    static TokenList tokens;  // Large, so keep these off the stack.
    static Statement stmt;
    ParamList param_list = { params, param_count };

    if (!tokenise(sql, &tokens, error) ||
        !parse(&tokens, &stmt, error) ||
        !execute(db, &stmt, &param_list, result, error)) {
        return 0;
    }

    // EXPLAIN changes nothing, so there's nothing to save.
    if (!stmt.explain &&
        !update_file(db, path, stmt.type, error)) {
        result_free(result);
        return 0;
    }

    return 1;
}
