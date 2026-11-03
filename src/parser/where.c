#include "internal.h"

#include "error.h"
#include "text.h"

// comparison = IDENTIFIER compare_op value
//            | IDENTIFIER IS [ NOT ] NULL
// compare_op = EQUAL | NOT_EQUAL | LESS | LESS_EQUAL
//            | GREATER | GREATER_EQUAL
static int parse_comparison(Parser *p, Comparison *out) {
    const Token *column = expect(p, TOKEN_IDENTIFIER,
                                 "column name, NOT or '('");
    if (!column) {
        return 0;
    }
    copy_text(out->column, sizeof(out->column), column->text);

    if (match(p, TOKEN_IS)) {
        int negated = match(p, TOKEN_NOT);

        out->op = negated ? CMP_IS_NOT_NULL : CMP_IS_NULL;
        out->value = literal_null();
        return expect(p, TOKEN_NULL, negated ? "NULL" : "NOT or NULL") != NULL;
    }

    switch (peek(p)->type) {
        case TOKEN_EQUAL:         out->op = CMP_EQUAL; break;
        case TOKEN_NOT_EQUAL:     out->op = CMP_NOT_EQUAL; break;
        case TOKEN_LESS:          out->op = CMP_LESS; break;
        case TOKEN_LESS_EQUAL:    out->op = CMP_LESS_EQUAL; break;
        case TOKEN_GREATER:       out->op = CMP_GREATER; break;
        case TOKEN_GREATER_EQUAL: out->op = CMP_GREATER_EQUAL; break;

        default:
            error_expected(p, "comparison (=, !=, <, <=, >, >= or IS)");
            return 0;
    }
    p->pos++;

    if (!parse_value(p, &out->value)) {
        return 0;
    }

    // In SQL, `age = NULL` matches no row, not even one whose age is
    // NULL. That's a common mistake, so it's an error here instead. A
    // `?` whose value is NULL isn't known yet, so it matches no row.
    if (out->value.kind == LITERAL_NULL) {
        return fail(p->error, sizeof(p->error),
                    "use IS NULL or IS NOT NULL to test for NULL");
    }

    return 1;
}

// Claims the next free condition node and sets its type. Children are
// claimed before their parent, so a parent's index is always higher.
static int new_condition(Parser *p, WhereClause *where,
                         ConditionType type, int *out) {
    if (where->condition_count == MAX_CONDITIONS) {
        return fail(p->error, sizeof(p->error),
                    "WHERE is too complex (max %d conditions)",
                    MAX_CONDITIONS);
    }

    *out = where->condition_count++;
    where->conditions[*out].type = type;
    return 1;
}

// Claims an AND or OR node joining left and right.
static int join_conditions(Parser *p, WhereClause *where,
                           ConditionType type, int left, int right,
                           int *out) {
    if (!new_condition(p, where, type, out)) {
        return 0;
    }

    where->conditions[*out].as.children.left = left;
    where->conditions[*out].as.children.right = right;
    return 1;
}

// The condition rules call each other, from loosest to tightest binding.
// Each one fills *out with the index of the node it built.
static int parse_condition(Parser *p, WhereClause *where, int *out);

// not_condition = NOT not_condition
//               | LPAREN condition RPAREN
//               | comparison
static int parse_not_condition(Parser *p, WhereClause *where,
                               int *out) {
    if (match(p, TOKEN_NOT)) {
        int operand;

        if (!parse_not_condition(p, where, &operand) ||
            !new_condition(p, where, COND_NOT, out)) {
            return 0;
        }

        where->conditions[*out].as.children.left = operand;
        return 1;
    }

    if (match(p, TOKEN_LPAREN)) {
        return parse_condition(p, where, out) &&
               expect(p, TOKEN_RPAREN, "AND, OR or ')'") != NULL;
    }

    return new_condition(p, where, COND_COMPARE, out) &&
           parse_comparison(p, &where->conditions[*out].as.compare);
}

// and_condition = not_condition { AND not_condition }
static int parse_and_condition(Parser *p, WhereClause *where,
                               int *out) {
    if (!parse_not_condition(p, where, out)) {
        return 0;
    }

    // Each AND takes everything so far as its left side, so
    // `a AND b AND c` groups as `(a AND b) AND c`.
    while (match(p, TOKEN_AND)) {
        int right;

        if (!parse_not_condition(p, where, &right) ||
            !join_conditions(p, where, COND_AND, *out, right, out)) {
            return 0;
        }
    }

    return 1;
}

// condition = and_condition { OR and_condition }
//
// OR is parsed outermost, so AND binds tighter:
// `a OR b AND c` means `a OR (b AND c)`.
static int parse_condition(Parser *p, WhereClause *where, int *out) {
    if (!parse_and_condition(p, where, out)) {
        return 0;
    }

    while (match(p, TOKEN_OR)) {
        int right;

        if (!parse_and_condition(p, where, &right) ||
            !join_conditions(p, where, COND_OR, *out, right, out)) {
            return 0;
        }
    }

    return 1;
}

int parse_where(Parser *p, int *has_where, WhereClause *where) {
    *has_where = match(p, TOKEN_WHERE);
    where->condition_count = 0;

    return !*has_where || parse_condition(p, where, &where->root);
}
