#include "internal.h"

// transaction_stmt = BEGIN | COMMIT | ROLLBACK
int parse_transaction(Parser *p, Statement *out) {
    if (match(p, TOKEN_BEGIN)) {
        out->type = STMT_BEGIN;
    } else if (match(p, TOKEN_COMMIT)) {
        out->type = STMT_COMMIT;
    } else if (match(p, TOKEN_ROLLBACK)) {
        out->type = STMT_ROLLBACK;
    } else {
        error_expected(p, "BEGIN, COMMIT or ROLLBACK");
        return 0;
    }

    out->table_name[0] = '\0';
    return 1;
}
