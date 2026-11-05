#include "internal.h"

#include "error.h"
#include "text.h"

// statement = ( create_stmt | insert_stmt | select_stmt | delete_stmt
//             | update_stmt | drop_stmt | transaction_stmt
//             | EXPLAIN ( select_stmt | update_stmt | delete_stmt ) )
//             SEMICOLON EOF
int parse(const TokenList *tokens, Statement *out,
          char *error, size_t error_size) {
    Parser p = { tokens, 0, 0, "", out };
    int ok;

    out->node_count = 0;
    out->explain = match(&p, TOKEN_EXPLAIN);

    switch (peek(&p)->type) {
        case TOKEN_CREATE:
            ok = parse_create(&p, out);
            break;

        case TOKEN_INSERT:
            ok = parse_insert(&p, out);
            break;

        case TOKEN_SELECT:
            ok = parse_select(&p, out);
            break;

        case TOKEN_DELETE:
            ok = parse_delete(&p, out);
            break;

        case TOKEN_UPDATE:
            ok = parse_update(&p, out);
            break;

        case TOKEN_DROP:
            ok = parse_drop(&p, out);
            break;

        case TOKEN_BEGIN:
        case TOKEN_COMMIT:
        case TOKEN_ROLLBACK:
            ok = parse_transaction(&p, out);
            break;

        default:
            error_expected(&p, "CREATE, INSERT, SELECT, UPDATE, DELETE, "
                               "DROP, BEGIN, COMMIT, ROLLBACK or EXPLAIN");
            ok = 0;
            break;
    }

    ok = ok &&
         expect(&p, TOKEN_SEMICOLON, "';'") != NULL &&
         expect(&p, TOKEN_EOF, "end of input") != NULL;

    // Only these find rows, so only they have a plan to explain.
    if (ok && out->explain && out->type != STMT_SELECT &&
        out->type != STMT_UPDATE && out->type != STMT_DELETE) {
        ok = fail(p.error, sizeof(p.error),
                  "EXPLAIN works on SELECT, UPDATE and DELETE");
    }

    if (!ok) {
        copy_text(error, error_size, p.error);
        return 0;
    }

    out->param_count = p.param_count;
    return 1;
}
