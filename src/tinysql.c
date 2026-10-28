#include "tinysql.h"

#include "error.h"
#include "executor.h"
#include "parser.h"
#include "tokeniser.h"

int tinysql_run(Database *db, const char *path, const char *sql,
                Result *result, char *error, size_t error_size) {
    static TokenList tokens;  // Large, so keep these off the stack.
    static Statement stmt;

    if (!tokenise(sql, &tokens, error, error_size) ||
        !parse(&tokens, &stmt, error, error_size) ||
        !execute(db, &stmt, result, error, error_size)) {
        return 0;
    }

    // Rewrite the file after every change, so stopping the program at
    // any point loses nothing that succeeded.
    if (path && stmt.type != STMT_SELECT) {
        char reason[TINYSQL_MAX_ERROR];

        if (!database_save(db, path, reason, sizeof(reason))) {
            result_free(result);
            return fail(error, error_size,
                        "change kept in memory but not saved: %s", reason);
        }
    }

    return 1;
}
