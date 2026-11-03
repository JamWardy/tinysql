#include "internal.h"

#include "error.h"
#include "text.h"

// assignment = IDENTIFIER EQUAL expr
static int parse_assignment(Parser *p, Assignment *out) {
    const Token *column = expect(p, TOKEN_IDENTIFIER, "column name");
    if (!column) {
        return 0;
    }

    if (!expect(p, TOKEN_EQUAL, "'='") ||
        !parse_expr(p, &out->value)) {
        return 0;
    }

    copy_text(out->column, sizeof(out->column), column->text);
    return 1;
}

// update_stmt = UPDATE IDENTIFIER
//               SET assignment { COMMA assignment } [ WHERE condition ]
int parse_update(Parser *p, Statement *out) {
    UpdateStatement *update = &out->as.update;

    if (!expect(p, TOKEN_UPDATE, "UPDATE")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    if (!expect(p, TOKEN_SET, "SET")) {
        return 0;
    }

    update->assignment_count = 0;

    do {
        if (update->assignment_count == MAX_COLUMNS) {
            return fail(p->error, sizeof(p->error),
                        "too many assignments (max %d)", MAX_COLUMNS);
        }

        if (!parse_assignment(
                p, &update->assignments[update->assignment_count])) {
            return 0;
        }

        update->assignment_count++;
    } while (match(p, TOKEN_COMMA));

    if (!parse_where(p, &update->has_where, &update->where) ||
        !expect_end(p, update->has_where ? "AND, OR or ';'"
                                         : "',', WHERE or ';'")) {
        return 0;
    }

    out->type = STMT_UPDATE;
    copy_text(out->table_name, sizeof(out->table_name), name->text);
    return 1;
}
