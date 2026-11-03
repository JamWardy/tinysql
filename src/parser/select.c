#include "internal.h"

#include <errno.h>
#include <stdlib.h>

#include "error.h"
#include "text.h"

static int is_aggregate_start(const Parser *p) {
    switch (peek(p)->type) {
        case TOKEN_COUNT:
        case TOKEN_SUM:
        case TOKEN_MIN:
        case TOKEN_MAX:
            return 1;
        default:
            return 0;
    }
}

static void error_mixed(Parser *p) {
    fail(p->error, sizeof(p->error),
         "can't mix columns and aggregates (there's no GROUP BY)");
}

// aggregate = COUNT LPAREN ( STAR | IDENTIFIER ) RPAREN
//           | ( SUM | MIN | MAX ) LPAREN IDENTIFIER RPAREN
static int parse_aggregate(Parser *p, Aggregate *out) {
    switch (peek(p)->type) {
        case TOKEN_COUNT: out->func = AGG_COUNT; break;
        case TOKEN_SUM:   out->func = AGG_SUM; break;
        case TOKEN_MIN:   out->func = AGG_MIN; break;
        case TOKEN_MAX:   out->func = AGG_MAX; break;

        default:
            // A column name here is valid on its own, so name the
            // real problem rather than what could have come instead.
            if (peek(p)->type == TOKEN_IDENTIFIER) {
                error_mixed(p);
            } else {
                error_expected(p, "COUNT, SUM, MIN or MAX");
            }
            return 0;
    }
    p->pos++;

    if (!expect(p, TOKEN_LPAREN, "'('")) {
        return 0;
    }

    if (out->func == AGG_COUNT && match(p, TOKEN_STAR)) {
        out->column[0] = '\0';
    } else {
        const Token *column = expect(p, TOKEN_IDENTIFIER,
                                     out->func == AGG_COUNT
                                         ? "'*' or column name"
                                         : "column name");
        if (!column) {
            return 0;
        }
        copy_text(out->column, sizeof(out->column), column->text);
    }

    return expect(p, TOKEN_RPAREN, "')'") != NULL;
}

// aggregate { COMMA aggregate }
static int parse_aggregate_list(Parser *p, SelectStatement *out) {
    do {
        if (out->aggregate_count == MAX_COLUMNS) {
            return fail(p->error, sizeof(p->error),
                        "too many aggregates (max %d)", MAX_COLUMNS);
        }

        if (!parse_aggregate(p, &out->aggregates[out->aggregate_count])) {
            return 0;
        }

        out->aggregate_count++;
    } while (match(p, TOKEN_COMMA));

    return 1;
}

// select_list = STAR
//             | aggregate { COMMA aggregate }
//             | IDENTIFIER { COMMA IDENTIFIER }
static int parse_select_list(Parser *p, SelectStatement *out) {
    out->column_count = 0;
    out->aggregate_count = 0;
    out->select_all = 0;
    out->has_aggregates = 0;

    if (match(p, TOKEN_STAR)) {
        out->select_all = 1;
        return 1;
    }

    // The first item decides which kind of list this is.
    if (is_aggregate_start(p)) {
        out->has_aggregates = 1;
        return parse_aggregate_list(p, out);
    }

    do {
        if (out->column_count == MAX_COLUMNS) {
            return fail(p->error, sizeof(p->error),
                        "too many columns (max %d)", MAX_COLUMNS);
        }

        if (is_aggregate_start(p)) {
            error_mixed(p);
            return 0;
        }

        const Token *column = expect(
            p, TOKEN_IDENTIFIER,
            out->column_count == 0 ? "'*' or column name" : "column name");
        if (!column) {
            return 0;
        }

        copy_text(out->columns[out->column_count],
                  sizeof(out->columns[out->column_count]), column->text);
        out->column_count++;
    } while (match(p, TOKEN_COMMA));

    return 1;
}

// [ ORDER BY IDENTIFIER [ ASC | DESC ] ]
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

    const Token *column = expect(p, TOKEN_IDENTIFIER, "column name");
    if (!column) {
        return 0;
    }
    copy_text(select->order_column, sizeof(select->order_column),
              column->text);

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
        return fail(p->error, sizeof(p->error),
                    "number out of range: %s", number->text);
    }

    select->limit = literal_int(limit);
    return 1;
}

// select_stmt = SELECT select_list FROM IDENTIFIER [ WHERE condition ]
//               [ ORDER BY IDENTIFIER [ ASC | DESC ] ]
//               [ LIMIT ( NUMBER | PARAM ) ]
int parse_select(Parser *p, Statement *out) {
    SelectStatement *select = &out->as.select;

    if (!expect(p, TOKEN_SELECT, "SELECT") ||
        !parse_select_list(p, select)) {
        return 0;
    }

    // After a column name, a comma would also have been valid.
    if (!expect(p, TOKEN_FROM, select->select_all ? "FROM" : "',' or FROM")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    int explicit_direction;

    if (!parse_where(p, &select->has_where, &select->where) ||
        !parse_order_by(p, select, &explicit_direction) ||
        !parse_limit(p, select)) {
        return 0;
    }

    // What could have continued the statement depends on the last
    // clause written.
    const char *options;

    if (select->has_limit) {
        options = "';'";
    } else if (select->has_order) {
        options = explicit_direction ? "LIMIT or ';'"
                                     : "ASC, DESC, LIMIT or ';'";
    } else if (select->has_where) {
        options = "AND, OR, ORDER BY, LIMIT or ';'";
    } else {
        options = "WHERE, ORDER BY, LIMIT or ';'";
    }

    if (!expect_end(p, options)) {
        return 0;
    }

    out->type = STMT_SELECT;
    copy_text(out->table_name, sizeof(out->table_name), name->text);
    return 1;
}
