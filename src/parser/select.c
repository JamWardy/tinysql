#include "internal.h"

#include <errno.h>
#include <stdlib.h>

#include "error.h"
#include "text.h"

// select_list = STAR | select_item { COMMA select_item }
// select_item = expr [ AS IDENTIFIER ]
int parse_select_list(Parser *p, SelectList *out) {
    out->item_count = 0;
    out->select_all = match(p, TOKEN_STAR);

    if (out->select_all) {
        return 1;
    }

    do {
        if (out->item_count == MAX_COLUMNS) {
            return fail(&p->error, ERROR_PROGRAM_LIMIT,
                        "too many columns (max %d)", MAX_COLUMNS);
        }

        SelectItem *item = &out->items[out->item_count];
        item->alias[0] = '\0';

        if (!parse_expr(p, &item->expr)) {
            return 0;
        }

        if (match(p, TOKEN_AS)) {
            const Token *alias = expect(p, TOKEN_IDENTIFIER, "name after AS");
            if (!alias) {
                return 0;
            }
            copy_text(item->alias, sizeof(item->alias), alias->text);
        }

        out->item_count++;
    } while (match(p, TOKEN_COMMA));

    return 1;
}

// [ ORDER BY expr [ ASC | DESC ] ]
// Sets *explicit_direction if ASC or DESC was written, for the error
// message in parse_select.
static int parse_order_by(Parser *p, SelectStatement *select,
                          int *explicit_direction) {
    select->has_order = match(p, TOKEN_ORDER);
    select->descending = 0;
    *explicit_direction = 0;

    if (!select->has_order) {
        return 1;
    }

    if (!expect(p, TOKEN_BY, "BY")) {
        return 0;
    }

    if (!parse_expr(p, &select->order_by)) {
        return 0;
    }

    if (match(p, TOKEN_DESC)) {
        select->descending = 1;
        *explicit_direction = 1;
    } else if (match(p, TOKEN_ASC)) {
        *explicit_direction = 1;
    }

    return 1;
}

// [ LIMIT ( NUMBER | PARAM ) ]
static int parse_limit(Parser *p, SelectStatement *select) {
    select->has_limit = match(p, TOKEN_LIMIT);

    if (!select->has_limit) {
        return 1;
    }

    // The executor checks a `?`'s value is an INT and not negative.
    if (match(p, TOKEN_PARAM)) {
        select->limit = literal_param(p);
        return 1;
    }

    // NUMBER is only digits, so a negative LIMIT fails here.
    const Token *number = expect(p, TOKEN_NUMBER, "number or ?");
    if (!number) {
        return 0;
    }

    errno = 0;
    long long limit = strtoll(number->text, NULL, 10);

    if (errno == ERANGE) {
        return fail(&p->error, ERROR_NUMERIC_OUT_OF_RANGE,
                    "number out of range: %s", number->text);
    }

    select->limit = literal_int(limit);
    return 1;
}

// [ FOR UPDATE ]
static int parse_for_update(Parser *p, SelectStatement *select) {
    select->for_update = match(p, TOKEN_FOR);

    return !select->for_update || expect(p, TOKEN_UPDATE, "UPDATE");
}

// select_stmt = SELECT select_list FROM IDENTIFIER [ WHERE expr ]
//               [ ORDER BY expr [ ASC | DESC ] ]
//               [ LIMIT ( NUMBER | PARAM ) ] [ FOR UPDATE ]
int parse_select(Parser *p, Statement *out) {
    SelectStatement *select = &out->as.select;

    if (!expect(p, TOKEN_SELECT, "SELECT") ||
        !parse_select_list(p, &select->list)) {
        return 0;
    }

    // After a column name, a comma would also have been valid.
    if (!expect(p, TOKEN_FROM,
                select->list.select_all ? "FROM" : "',' or FROM")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    int explicit_direction;

    if (!parse_where(p, &select->has_where, &select->where) ||
        !parse_order_by(p, select, &explicit_direction) ||
        !parse_limit(p, select) ||
        !parse_for_update(p, select)) {
        return 0;
    }

    // What could have continued the statement depends on the last
    // clause written.
    const char *options;

    if (select->for_update) {
        options = "';'";
    } else if (select->has_limit) {
        options = "FOR UPDATE or ';'";
    } else if (select->has_order) {
        options = explicit_direction ? "LIMIT, FOR UPDATE or ';'"
                                     : "ASC, DESC, LIMIT, FOR UPDATE or ';'";
    } else if (select->has_where) {
        options = "AND, OR, ORDER BY, LIMIT, FOR UPDATE or ';'";
    } else {
        options = "WHERE, ORDER BY, LIMIT, FOR UPDATE or ';'";
    }

    if (!expect_end(p, options)) {
        return 0;
    }

    out->type = STMT_SELECT;
    copy_text(out->table_name, sizeof(out->table_name), name->text);
    return 1;
}
