#include "internal.h"

#include <limits.h>
#include <stdio.h>

#include "error.h"

// Most messages name an expression, rebuilt as SQL and cut short to fit.
#define MAX_EXPR_TEXT 64

// A flag for expr_resolve's recursion only: inside an aggregate, which
// can't hold another.
#define EXPR_IN_AGGREGATE 4

static Value null_value(void) {
    Value value;
    value.type = TYPE_NULL;
    return value;
}

static Value bool_value(int b) {
    Value value;
    value.type = TYPE_BOOL;
    value.as.boolean = b;
    return value;
}

static Value int_value(long long n) {
    Value value;
    value.type = TYPE_INT;
    value.as.integer = n;
    return value;
}

void expr_init(ExprContext *ctx, const Statement *stmt, const Table *table,
               const ParamList *params) {
    ctx->nodes = stmt->nodes;
    ctx->table = table;
    ctx->params = params;
}

static int is_comparison(Operator op) {
    switch (op) {
        case OP_EQUAL:
        case OP_NOT_EQUAL:
        case OP_LESS:
        case OP_LESS_EQUAL:
        case OP_GREATER:
        case OP_GREATER_EQUAL:
            return 1;
        case OP_NEGATE:
        case OP_NOT:
        case OP_ADD:
        case OP_SUBTRACT:
        case OP_MULTIPLY:
        case OP_DIVIDE:
        case OP_REMAINDER:
        case OP_AND:
        case OP_OR:
            return 0;
    }
    return 0;
}

// NULL fits anything. what names the operator or function for the message.
static int check_operand(const ExprContext *ctx, int operand, DataType want,
                         const char *what, char *error, size_t error_size) {
    DataType type = ctx->types[operand];

    if (type == want || type == TYPE_NULL) {
        return 1;
    }

    char text[MAX_EXPR_TEXT];
    expr_to_text(ctx->nodes, operand, text, sizeof(text));
    return fail(error, error_size, "%s needs %s, but %s is %s", what,
                data_type_name(want), text, data_type_name(type));
}

static int resolve_node(ExprContext *ctx, int node, int allowed,
                        const char *place, char *error, size_t error_size);

static int resolve_aggregate(ExprContext *ctx, int node, int allowed,
                             const char *place,
                             char *error, size_t error_size) {
    const ExprNode *n = &ctx->nodes[node];
    AggregateFunc func = n->as.aggregate.func;
    int operand = n->as.aggregate.operand;

    if (allowed & EXPR_IN_AGGREGATE) {
        return fail(error, error_size,
                    "can't use an aggregate inside another");
    }
    if (!(allowed & EXPR_AGGREGATES)) {
        return fail(error, error_size, "can't use aggregates in %s", place);
    }

    // COUNT and SUM give an INT; MIN and MAX give their operand's type.
    ctx->types[node] = TYPE_INT;

    if (operand < 0) {
        return 1;  // COUNT(*)
    }

    if (!resolve_node(ctx, operand, EXPR_COLUMNS | EXPR_IN_AGGREGATE, place,
                      error, error_size)) {
        return 0;
    }

    DataType type = ctx->types[operand];

    switch (func) {
        case AGG_COUNT:
            return 1;
        case AGG_SUM:
            return check_operand(ctx, operand, TYPE_INT, "SUM",
                                 error, error_size);
        case AGG_MIN:
        case AGG_MAX:
            if (type == TYPE_BOOL) {
                char text[MAX_EXPR_TEXT];
                expr_to_text(ctx->nodes, operand, text, sizeof(text));
                return fail(error, error_size,
                            "%s needs INT or TEXT, but %s is BOOL",
                            aggregate_name(func), text);
            }
            ctx->types[node] = type;
            return 1;
    }
    return 1;
}

static int resolve_binary(ExprContext *ctx, int node, int allowed,
                          const char *place, char *error, size_t error_size) {
    const ExprNode *n = &ctx->nodes[node];
    Operator op = n->as.op.op;
    int left = n->as.op.left;
    int right = n->as.op.right;

    if (!resolve_node(ctx, left, allowed, place, error, error_size) ||
        !resolve_node(ctx, right, allowed, place, error, error_size)) {
        return 0;
    }

    if (is_comparison(op)) {
        DataType a = ctx->types[left];
        DataType b = ctx->types[right];

        if (a != b && a != TYPE_NULL && b != TYPE_NULL) {
            char left_text[MAX_EXPR_TEXT];
            char right_text[MAX_EXPR_TEXT];

            expr_to_text(ctx->nodes, left, left_text, sizeof(left_text));
            expr_to_text(ctx->nodes, right, right_text, sizeof(right_text));
            return fail(error, error_size, "can't compare %s (%s) with %s (%s)",
                        left_text, data_type_name(a),
                        right_text, data_type_name(b));
        }
        ctx->types[node] = TYPE_BOOL;
        return 1;
    }

    // AND and OR take BOOLs; the rest are arithmetic on INTs.
    DataType want = op == OP_AND || op == OP_OR ? TYPE_BOOL : TYPE_INT;
    char what[16];

    snprintf(what, sizeof(what), "'%s'", operator_name(op));
    if (!check_operand(ctx, left, want, what, error, error_size) ||
        !check_operand(ctx, right, want, what, error, error_size)) {
        return 0;
    }

    ctx->types[node] = want;
    return 1;
}

// place names where the expression is, such as "WHERE", for messages.
static int resolve_node(ExprContext *ctx, int node, int allowed,
                        const char *place, char *error, size_t error_size) {
    const ExprNode *n = &ctx->nodes[node];

    switch (n->kind) {
        case EXPR_LITERAL:
            ctx->types[node] = literal_type(&n->as.literal,
                                            ctx->params);
            return 1;

        case EXPR_COLUMN:
            if (!(allowed & EXPR_COLUMNS)) {
                if (allowed & EXPR_AGGREGATES) {
                    return fail(error, error_size,
                                "can't mix columns and aggregates "
                                "(there's no GROUP BY)");
                }
                return fail(error, error_size, "can't use column '%s' in %s",
                            n->as.column, place);
            }
            if (!resolve_column(ctx->table, n->as.column,
                                &ctx->columns[node], error, error_size)) {
                return 0;
            }
            ctx->types[node] = ctx->table->columns[ctx->columns[node]].type;
            return 1;

        case EXPR_UNARY: {
            int operand = n->as.op.left;
            DataType want = n->as.op.op == OP_NOT ? TYPE_BOOL : TYPE_INT;
            char what[16];

            snprintf(what, sizeof(what), "'%s'", operator_name(n->as.op.op));
            if (!resolve_node(ctx, operand, allowed, place,
                              error, error_size) ||
                !check_operand(ctx, operand, want, what,
                               error, error_size)) {
                return 0;
            }
            ctx->types[node] = want;
            return 1;
        }

        case EXPR_BINARY:
            return resolve_binary(ctx, node, allowed, place,
                                  error, error_size);

        case EXPR_IS_NULL:
            ctx->types[node] = TYPE_BOOL;
            return resolve_node(ctx, n->as.is_null.operand, allowed, place,
                                error, error_size);

        case EXPR_AGGREGATE:
            return resolve_aggregate(ctx, node, allowed, place,
                                     error, error_size);
    }
    return 1;
}

int expr_resolve(ExprContext *ctx, int node, int allowed, const char *place,
                 char *error, size_t error_size) {
    return resolve_node(ctx, node, allowed, place, error, error_size);
}

int expr_has_aggregate(const ExprNode *nodes, int node) {
    const ExprNode *n = &nodes[node];

    switch (n->kind) {
        case EXPR_LITERAL:
        case EXPR_COLUMN:
            return 0;
        case EXPR_UNARY:
            return expr_has_aggregate(nodes, n->as.op.left);
        case EXPR_BINARY:
            return expr_has_aggregate(nodes, n->as.op.left) ||
                   expr_has_aggregate(nodes, n->as.op.right);
        case EXPR_IS_NULL:
            return expr_has_aggregate(nodes, n->as.is_null.operand);
        case EXPR_AGGREGATE:
            return 1;
    }
    return 0;
}

int expr_uses_columns(const ExprNode *nodes, int node) {
    const ExprNode *n = &nodes[node];

    switch (n->kind) {
        case EXPR_LITERAL:
            return 0;
        case EXPR_COLUMN:
        case EXPR_AGGREGATE:
            return 1;
        case EXPR_UNARY:
            return expr_uses_columns(nodes, n->as.op.left);
        case EXPR_BINARY:
            return expr_uses_columns(nodes, n->as.op.left) ||
                   expr_uses_columns(nodes, n->as.op.right);
        case EXPR_IS_NULL:
            return expr_uses_columns(nodes, n->as.is_null.operand);
    }
    return 0;
}

static int out_of_range(char *error, size_t error_size) {
    return fail(error, error_size, "integer out of range");
}

// Sets *out to a op b, failing if the answer doesn't fit. Each check
// comes before the sum or product: in C, overflowing a signed integer is
// undefined behaviour, not a wrap-around.
static int arithmetic(Operator op, long long a, long long b, long long *out,
                      char *error, size_t error_size) {
    switch (op) {
        case OP_ADD:
            if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)) {
                return out_of_range(error, error_size);
            }
            *out = a + b;
            return 1;

        case OP_SUBTRACT:
            if ((b < 0 && a > LLONG_MAX + b) || (b > 0 && a < LLONG_MIN + b)) {
                return out_of_range(error, error_size);
            }
            *out = a - b;
            return 1;

        case OP_MULTIPLY:
            if (a > 0 ? (b > 0 ? a > LLONG_MAX / b : b < LLONG_MIN / a)
                      : (b > 0 ? a < LLONG_MIN / b
                               : a != 0 && b < LLONG_MAX / a)) {
                return out_of_range(error, error_size);
            }
            *out = a * b;
            return 1;

        case OP_DIVIDE:
        case OP_REMAINDER:
            if (b == 0) {
                return fail(error, error_size, "division by zero");
            }
            // The one quotient that doesn't fit: the most negative INT
            // has no positive counterpart. Its remainder is 0, but C
            // leaves that case undefined too.
            if (a == LLONG_MIN && b == -1) {
                if (op == OP_DIVIDE) {
                    return out_of_range(error, error_size);
                }
                *out = 0;
                return 1;
            }
            // C rounds toward zero, and gives the remainder the sign of a.
            *out = op == OP_DIVIDE ? a / b : a % b;
            return 1;

        case OP_NEGATE:
        case OP_NOT:
        case OP_EQUAL:
        case OP_NOT_EQUAL:
        case OP_LESS:
        case OP_LESS_EQUAL:
        case OP_GREATER:
        case OP_GREATER_EQUAL:
        case OP_AND:
        case OP_OR:
            break;  // Not arithmetic.
    }
    return 1;
}

static int compare(Operator op, const Value *a, const Value *b) {
    int c = value_compare(a, b);

    switch (op) {
        case OP_EQUAL:         return c == 0;
        case OP_NOT_EQUAL:     return c != 0;
        case OP_LESS:          return c < 0;
        case OP_LESS_EQUAL:    return c <= 0;
        case OP_GREATER:       return c > 0;
        case OP_GREATER_EQUAL: return c >= 0;
        case OP_NEGATE:
        case OP_NOT:
        case OP_ADD:
        case OP_SUBTRACT:
        case OP_MULTIPLY:
        case OP_DIVIDE:
        case OP_REMAINDER:
        case OP_AND:
        case OP_OR:
            break;  // Not comparisons.
    }
    return 0;
}

// AND and OR, in SQL's three-valued logic: NULL is unknown. Unknown
// matters only where it could change the answer: false AND unknown is
// false, and true OR unknown is true, whatever the unknown turns out to
// be. So the right side isn't evaluated when the left decides.
static int logic(const ExprContext *ctx, const ExprNode *n, const Row *row,
                 Value *out, char *error, size_t error_size) {
    int decides = n->as.op.op == OP_OR;  // The value that decides alone.
    Value left;
    Value right;

    if (!expr_evaluate(ctx, n->as.op.left, row, &left, error, error_size)) {
        return 0;
    }
    if (left.type == TYPE_BOOL && left.as.boolean == decides) {
        *out = bool_value(decides);
        return 1;
    }

    if (!expr_evaluate(ctx, n->as.op.right, row, &right, error, error_size)) {
        return 0;
    }
    if (right.type == TYPE_BOOL && right.as.boolean == decides) {
        *out = bool_value(decides);
        return 1;
    }

    // Neither side decides: both are the other value, or one is unknown.
    *out = left.type == TYPE_NULL || right.type == TYPE_NULL
               ? null_value() : bool_value(!decides);
    return 1;
}

static int evaluate_binary(const ExprContext *ctx, const ExprNode *n,
                           const Row *row, Value *out,
                           char *error, size_t error_size) {
    Operator op = n->as.op.op;

    if (op == OP_AND || op == OP_OR) {
        return logic(ctx, n, row, out, error, error_size);
    }

    Value left;
    Value right;

    if (!expr_evaluate(ctx, n->as.op.left, row, &left, error, error_size) ||
        !expr_evaluate(ctx, n->as.op.right, row, &right, error, error_size)) {
        return 0;
    }

    if (left.type == TYPE_NULL || right.type == TYPE_NULL) {
        *out = null_value();
        return 1;
    }

    if (is_comparison(op)) {
        *out = bool_value(compare(op, &left, &right));
        return 1;
    }

    long long n_out;
    if (!arithmetic(op, left.as.integer, right.as.integer, &n_out,
                    error, error_size)) {
        return 0;
    }
    *out = int_value(n_out);
    return 1;
}

int expr_evaluate(const ExprContext *ctx, int node, const Row *row,
                  Value *out, char *error, size_t error_size) {
    const ExprNode *n = &ctx->nodes[node];

    switch (n->kind) {
        case EXPR_LITERAL:
            *out = literal_to_value(&n->as.literal, ctx->params);
            return 1;

        case EXPR_COLUMN:
            *out = row->values[ctx->columns[node]];
            return 1;

        case EXPR_AGGREGATE:
            *out = ctx->aggregates[node];
            return 1;

        case EXPR_IS_NULL: {
            Value operand;

            if (!expr_evaluate(ctx, n->as.is_null.operand, row, &operand,
                               error, error_size)) {
                return 0;
            }
            *out = bool_value((operand.type == TYPE_NULL) !=
                              n->as.is_null.negated);
            return 1;
        }

        case EXPR_UNARY: {
            Value operand;

            if (!expr_evaluate(ctx, n->as.op.left, row, &operand,
                               error, error_size)) {
                return 0;
            }
            if (operand.type == TYPE_NULL) {
                *out = null_value();
            } else if (n->as.op.op == OP_NOT) {
                *out = bool_value(!operand.as.boolean);
            } else if (operand.as.integer == LLONG_MIN) {
                return out_of_range(error, error_size);
            } else {
                *out = int_value(-operand.as.integer);
            }
            return 1;
        }

        case EXPR_BINARY:
            return evaluate_binary(ctx, n, row, out, error, error_size);
    }
    return 1;
}

// Sets *out to the aggregate at node over rows. COUNT(*) counts rows,
// NULLs and all, and COUNT(x) the rows where x isn't NULL; both give 0
// when there are none. SUM, MIN and MAX skip NULLs, and give NULL when
// there's no other value: no rows, or only NULLs. Text in *out points
// into a row or the statement.
static int compute_aggregate(const ExprContext *ctx, int node,
                             const RowList *rows, Value *out,
                             char *error, size_t error_size) {
    const ExprNode *n = &ctx->nodes[node];
    AggregateFunc func = n->as.aggregate.func;
    int operand = n->as.aggregate.operand;

    if (operand < 0) {
        *out = int_value((long long)rows->count);
        return 1;
    }

    long long count = 0;
    *out = null_value();

    for (size_t r = 0; r < rows->count; r++) {
        Value value;

        if (!expr_evaluate(ctx, operand, rows->rows[r], &value,
                           error, error_size)) {
            return 0;
        }
        if (value.type == TYPE_NULL) {
            continue;
        }
        count++;

        // The first value found starts the result.
        if (out->type == TYPE_NULL) {
            *out = value;
            continue;
        }

        switch (func) {
            case AGG_SUM:
                if (!arithmetic(OP_ADD, out->as.integer, value.as.integer,
                                &out->as.integer, error, error_size)) {
                    return 0;
                }
                break;
            case AGG_MIN:
                if (value_compare(&value, out) < 0) {
                    *out = value;
                }
                break;
            case AGG_MAX:
                if (value_compare(&value, out) > 0) {
                    *out = value;
                }
                break;
            case AGG_COUNT:
                break;  // Counted above.
        }
    }

    if (func == AGG_COUNT) {
        *out = int_value(count);
    }
    return 1;
}

int expr_compute_aggregates(ExprContext *ctx, int node, const RowList *rows,
                            char *error, size_t error_size) {
    const ExprNode *n = &ctx->nodes[node];

    switch (n->kind) {
        case EXPR_LITERAL:
        case EXPR_COLUMN:
            return 1;
        case EXPR_UNARY:
            return expr_compute_aggregates(ctx, n->as.op.left, rows,
                                           error, error_size);
        case EXPR_BINARY:
            return expr_compute_aggregates(ctx, n->as.op.left, rows,
                                           error, error_size) &&
                   expr_compute_aggregates(ctx, n->as.op.right, rows,
                                           error, error_size);
        case EXPR_IS_NULL:
            return expr_compute_aggregates(ctx, n->as.is_null.operand, rows,
                                           error, error_size);
        case EXPR_AGGREGATE:
            return compute_aggregate(ctx, node, rows, &ctx->aggregates[node],
                                     error, error_size);
    }
    return 1;
}
