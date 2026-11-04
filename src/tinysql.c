#include "tinysql.h"

#include "error.h"
#include "executor.h"
#include "parser.h"
#include "tokeniser.h"
#include "transaction.h"

// Does what a statement needs from the file once it has run: BEGIN,
// COMMIT and ROLLBACK do their work here, as the executor never sees
// the file, and any other change is autocommitted.
static int update_file(Database *db, const char *path, StatementType type,
                       char *error, size_t error_size) {
    switch (type) {
        case STMT_BEGIN:
            return transaction_begin(db, path, error, error_size);
        case STMT_COMMIT:
            return transaction_commit(db, path, error, error_size);
        case STMT_ROLLBACK:
            return transaction_rollback(db, path, error, error_size);
        case STMT_SELECT:
            return 1;
        case STMT_CREATE:
        case STMT_INSERT:
        case STMT_DELETE:
        case STMT_UPDATE:
        case STMT_DROP:
            return transaction_autocommit(db, path, error, error_size);
    }

    return fail(error, error_size, "unknown statement type");
}

int tinysql_run(Database *db, const char *path, const char *sql,
                const Value *params, size_t param_count,
                Result *result, char *error, size_t error_size) {
    static TokenList tokens;  // Large, so keep these off the stack.
    static Statement stmt;
    ParamList param_list = { params, param_count };

    if (!tokenise(sql, &tokens, error, error_size) ||
        !parse(&tokens, &stmt, error, error_size) ||
        !execute(db, &stmt, &param_list, result, error, error_size)) {
        return 0;
    }

    if (!update_file(db, path, stmt.type, error, error_size)) {
        result_free(result);
        return 0;
    }

    return 1;
}
