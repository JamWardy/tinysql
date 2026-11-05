#include "internal.h"

#include "error.h"
#include "text.h"

// insert_stmt = INSERT INTO IDENTIFIER
//               [ LPAREN IDENTIFIER { COMMA IDENTIFIER } RPAREN ]
//               VALUES LPAREN expr { COMMA expr } RPAREN
//               [ RETURNING select_list ]
int parse_insert(Parser *p, Statement *out) {
    if (!expect(p, TOKEN_INSERT, "INSERT") ||
        !expect(p, TOKEN_INTO, "INTO")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    InsertStatement *insert = &out->as.insert;
    insert->column_count = 0;
    insert->value_count = 0;

    if (match(p, TOKEN_LPAREN)) {
        do {
            if (insert->column_count == MAX_COLUMNS) {
                return fail(&p->error, ERROR_PROGRAM_LIMIT,
                            "too many columns (max %d)", MAX_COLUMNS);
            }

            const Token *column = expect(p, TOKEN_IDENTIFIER, "column name");
            if (!column) {
                return 0;
            }

            copy_text(insert->columns[insert->column_count],
                      sizeof(insert->columns[0]), column->text);
            insert->column_count++;
        } while (match(p, TOKEN_COMMA));

        if (!expect(p, TOKEN_RPAREN, "',' or ')'")) {
            return 0;
        }
    }

    if (!expect(p, TOKEN_VALUES,
                insert->column_count > 0 ? "VALUES" : "'(' or VALUES") ||
        !expect(p, TOKEN_LPAREN, "'('")) {
        return 0;
    }

    do {
        if (insert->value_count == MAX_COLUMNS) {
            return fail(&p->error, ERROR_PROGRAM_LIMIT,
                        "too many values (max %d)", MAX_COLUMNS);
        }

        if (!parse_expr(p, &insert->values[insert->value_count])) {
            return 0;
        }

        insert->value_count++;
    } while (match(p, TOKEN_COMMA));

    if (!expect(p, TOKEN_RPAREN, "',' or ')'")) {
        return 0;
    }

    if (insert->column_count > 0 &&
        insert->column_count != insert->value_count) {
        return fail(&p->error, ERROR_SYNTAX,
                    "%d columns were named but %d values were given",
                    insert->column_count, insert->value_count);
    }

    insert->has_returning = match(p, TOKEN_RETURNING);
    if (insert->has_returning &&
        !parse_select_list(p, &insert->returning)) {
        return 0;
    }

    const char *options = "RETURNING or ';'";

    if (insert->has_returning) {
        options = insert->returning.select_all ? "';'" : "',' or ';'";
    }
    if (!expect_end(p, options)) {
        return 0;
    }

    out->type = STMT_INSERT;
    copy_text(out->table_name, sizeof(out->table_name), name->text);
    return 1;
}
