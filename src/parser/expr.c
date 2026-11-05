#include "internal.h"

#include "error.h"
#include "text.h"

// The rules call each other from loosest to tightest binding, so each
// operator's operands are parsed by the rule for the next level down.
// Each fills *out with the index of the node it built. Children are
// built before their parent, so a parent's index is always higher.

static ExprNode *node_at(Parser *p, int index) {
    return &p->stmt->nodes[index];
}

int new_node(Parser *p, ExprKind kind, int *out) {
    Statement *stmt = p->stmt;

    if (stmt->node_count == MAX_NODES) {
        return fail(p->error, sizeof(p->error),
                    "statement is too complex (max %d expression nodes)",
                    MAX_NODES);
    }

    *out = stmt->node_count++;
    stmt->nodes[*out].kind = kind;
    return 1;
}

static int new_unary(Parser *p, Operator op, int operand, int *out) {
    if (!new_node(p, EXPR_UNARY, out)) {
        return 0;
    }

    ExprNode *node = node_at(p, *out);
    node->as.op.op = op;
    node->as.op.left = operand;
    node->as.op.right = -1;
    return 1;
}

static int new_binary(Parser *p, Operator op, int left, int right, int *out) {
    if (!new_node(p, EXPR_BINARY, out)) {
        return 0;
    }

    ExprNode *node = node_at(p, *out);
    node->as.op.op = op;
    node->as.op.left = left;
    node->as.op.right = right;
    return 1;
}

static int is_null_literal(Parser *p, int index) {
    const ExprNode *node = node_at(p, index);

    return node->kind == EXPR_LITERAL &&
           node->as.literal.kind == LITERAL_NULL;
}

// aggregate = COUNT LPAREN ( STAR | expr ) RPAREN
//           | ( SUM | MIN | MAX ) LPAREN expr RPAREN
static int parse_aggregate(Parser *p, int *out) {
    AggregateFunc func;

    switch (peek(p)->type) {
        case TOKEN_COUNT: func = AGG_COUNT; break;
        case TOKEN_SUM:   func = AGG_SUM; break;
        case TOKEN_MIN:   func = AGG_MIN; break;
        default:          func = AGG_MAX; break;  // parse_primary checked.
    }
    p->pos++;

    if (!expect(p, TOKEN_LPAREN, "'('")) {
        return 0;
    }

    int operand = -1;

    if (!(func == AGG_COUNT && match(p, TOKEN_STAR)) &&
        !parse_expr(p, &operand)) {
        return 0;
    }

    if (!expect(p, TOKEN_RPAREN, "')'") ||
        !new_node(p, EXPR_AGGREGATE, out)) {
        return 0;
    }

    ExprNode *node = node_at(p, *out);
    node->as.aggregate.func = func;
    node->as.aggregate.operand = operand;
    return 1;
}

// primary = value | IDENTIFIER | aggregate | LPAREN expr RPAREN
static int parse_primary(Parser *p, int *out) {
    const Token *token = peek(p);

    switch (token->type) {
        case TOKEN_LPAREN:
            p->pos++;
            return parse_expr(p, out) &&
                   expect(p, TOKEN_RPAREN, "')'") != NULL;

        case TOKEN_IDENTIFIER:
            if (!new_node(p, EXPR_COLUMN, out)) {
                return 0;
            }
            copy_text(node_at(p, *out)->as.column,
                      sizeof(node_at(p, *out)->as.column), token->text);
            p->pos++;
            return 1;

        case TOKEN_COUNT:
        case TOKEN_SUM:
        case TOKEN_MIN:
        case TOKEN_MAX:
            return parse_aggregate(p, out);

        case TOKEN_MINUS:
        case TOKEN_NUMBER:
        case TOKEN_STRING:
        case TOKEN_TRUE:
        case TOKEN_FALSE:
        case TOKEN_NULL:
        case TOKEN_PARAM:
            return new_node(p, EXPR_LITERAL, out) &&
                   parse_value(p, &node_at(p, *out)->as.literal);

        default:
            error_expected(p, "value, column or '('");
            return 0;
    }
}

// unary = MINUS unary | primary
//
// A MINUS right before a NUMBER is part of the number instead, so
// -9223372036854775808, whose digits alone don't fit, can be written.
static int parse_unary(Parser *p, int *out) {
    // The list ends in EOF, which isn't a MINUS, so pos + 1 exists.
    if (peek(p)->type == TOKEN_MINUS &&
        p->tokens->tokens[p->pos + 1].type != TOKEN_NUMBER) {
        int operand;

        p->pos++;
        return parse_unary(p, &operand) &&
               new_unary(p, OP_NEGATE, operand, out);
    }

    return parse_primary(p, out);
}

// multiplicative = unary { ( STAR | SLASH | PERCENT ) unary }
static int parse_multiplicative(Parser *p, int *out) {
    if (!parse_unary(p, out)) {
        return 0;
    }

    while (1) {
        Operator op;

        switch (peek(p)->type) {
            case TOKEN_STAR:    op = OP_MULTIPLY; break;
            case TOKEN_SLASH:   op = OP_DIVIDE; break;
            case TOKEN_PERCENT: op = OP_REMAINDER; break;
            default:            return 1;
        }
        p->pos++;

        // Each operator takes everything so far as its left side, so
        // `a / b * c` groups as `(a / b) * c`.
        int right;
        if (!parse_unary(p, &right) ||
            !new_binary(p, op, *out, right, out)) {
            return 0;
        }
    }
}

// additive = multiplicative { ( PLUS | MINUS ) multiplicative }
static int parse_additive(Parser *p, int *out) {
    if (!parse_multiplicative(p, out)) {
        return 0;
    }

    while (1) {
        Operator op;

        switch (peek(p)->type) {
            case TOKEN_PLUS:  op = OP_ADD; break;
            case TOKEN_MINUS: op = OP_SUBTRACT; break;
            default:          return 1;
        }
        p->pos++;

        int right;
        if (!parse_multiplicative(p, &right) ||
            !new_binary(p, op, *out, right, out)) {
            return 0;
        }
    }
}

// The rest of `a [NOT] LIKE p [ESCAPE e]`, a being operand, from the NOT,
// LIKE or ILIKE.
static int parse_like(Parser *p, int operand, int *out) {
    int negated = match(p, TOKEN_NOT);
    int ignore_case = peek(p)->type == TOKEN_ILIKE;
    int pattern;
    int escape = -1;

    p->pos++;  // LIKE or ILIKE, which parse_comparison checked.
    if (!parse_additive(p, &pattern) ||
        (match(p, TOKEN_ESCAPE) && !parse_additive(p, &escape)) ||
        !new_node(p, EXPR_LIKE, out)) {
        return 0;
    }

    ExprNode *node = node_at(p, *out);
    node->as.like.operand = operand;
    node->as.like.pattern = pattern;
    node->as.like.escape = escape;
    node->as.like.negated = negated;
    node->as.like.ignore_case = ignore_case;
    return 1;
}

static int is_like(TokenType type) {
    return type == TOKEN_LIKE || type == TOKEN_ILIKE;
}

// comparison = additive [ compare_op additive | IS [ NOT ] NULL
//                       | [ NOT ] ( LIKE | ILIKE ) additive
//                         [ ESCAPE additive ] ]
// compare_op = EQUAL | NOT_EQUAL | LESS | LESS_EQUAL
//            | GREATER | GREATER_EQUAL
//
// At most one comparison, so `a < b < c` is an error.
static int parse_comparison(Parser *p, int *out) {
    if (!parse_additive(p, out)) {
        return 0;
    }

    // The list ends in EOF, which isn't a NOT, so pos + 1 exists.
    if (is_like(peek(p)->type) ||
        (peek(p)->type == TOKEN_NOT &&
         is_like(p->tokens->tokens[p->pos + 1].type))) {
        return parse_like(p, *out, out);
    }

    if (match(p, TOKEN_IS)) {
        int negated = match(p, TOKEN_NOT);
        int operand = *out;

        if (!expect(p, TOKEN_NULL, negated ? "NULL" : "NOT or NULL") ||
            !new_node(p, EXPR_IS_NULL, out)) {
            return 0;
        }

        node_at(p, *out)->as.is_null.operand = operand;
        node_at(p, *out)->as.is_null.negated = negated;
        return 1;
    }

    Operator op;

    switch (peek(p)->type) {
        case TOKEN_EQUAL:         op = OP_EQUAL; break;
        case TOKEN_NOT_EQUAL:     op = OP_NOT_EQUAL; break;
        case TOKEN_LESS:          op = OP_LESS; break;
        case TOKEN_LESS_EQUAL:    op = OP_LESS_EQUAL; break;
        case TOKEN_GREATER:       op = OP_GREATER; break;
        case TOKEN_GREATER_EQUAL: op = OP_GREATER_EQUAL; break;
        default:                  return 1;
    }
    p->pos++;

    int right;
    if (!parse_additive(p, &right)) {
        return 0;
    }

    // In SQL, `age = NULL` matches no row, not even one whose age is
    // NULL. That's a common mistake, so it's an error here instead.
    if (is_null_literal(p, *out) || is_null_literal(p, right)) {
        return fail(p->error, sizeof(p->error),
                    "use IS NULL or IS NOT NULL to test for NULL");
    }

    return new_binary(p, op, *out, right, out);
}

// not_expr = NOT not_expr | comparison
static int parse_not(Parser *p, int *out) {
    if (match(p, TOKEN_NOT)) {
        int operand;

        return parse_not(p, &operand) &&
               new_unary(p, OP_NOT, operand, out);
    }

    return parse_comparison(p, out);
}

// and_expr = not_expr { AND not_expr }
static int parse_and(Parser *p, int *out) {
    if (!parse_not(p, out)) {
        return 0;
    }

    while (match(p, TOKEN_AND)) {
        int right;

        if (!parse_not(p, &right) ||
            !new_binary(p, OP_AND, *out, right, out)) {
            return 0;
        }
    }

    return 1;
}

// expr = and_expr { OR and_expr }
int parse_expr(Parser *p, int *out) {
    if (!parse_and(p, out)) {
        return 0;
    }

    while (match(p, TOKEN_OR)) {
        int right;

        if (!parse_and(p, &right) ||
            !new_binary(p, OP_OR, *out, right, out)) {
            return 0;
        }
    }

    return 1;
}

int parse_where(Parser *p, int *has_where, int *where) {
    *has_where = match(p, TOKEN_WHERE);
    *where = -1;

    return !*has_where || parse_expr(p, where);
}
