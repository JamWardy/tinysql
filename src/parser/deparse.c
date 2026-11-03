#include "parser.h"

#include <stdarg.h>
#include <stdio.h>

// How tightly each kind of node binds, loosest first, as in the grammar.
enum {
    PREC_OR = 1,
    PREC_AND,
    PREC_NOT,
    PREC_COMPARE,
    PREC_ADD,
    PREC_MULTIPLY,
    PREC_NEGATE,
    PREC_PRIMARY
};

// Text being written into a fixed buffer, cut short when it's full.
typedef struct {
    char *out;
    size_t size;
    size_t length;  // What's been written, which may pass size.
} Writer;

static void write_text(Writer *w, const char *format, ...) {
    va_list args;
    size_t room = w->length < w->size ? w->size - w->length : 0;

    va_start(args, format);
    // With no room left, vsnprintf writes nothing and only counts.
    int written = vsnprintf(room > 0 ? w->out + w->length : NULL, room,
                            format, args);
    va_end(args);

    w->length += written > 0 ? (size_t)written : 0;
}

static int precedence(const ExprNode *node) {
    switch (node->kind) {
        case EXPR_LITERAL:
            // -5 reads like a negation, so it's bracketed like one.
            return node->as.literal.kind == LITERAL_INT &&
                           node->as.literal.as.integer < 0
                       ? PREC_NEGATE : PREC_PRIMARY;
        case EXPR_COLUMN:
        case EXPR_AGGREGATE:
            return PREC_PRIMARY;
        case EXPR_IS_NULL:
            return PREC_COMPARE;
        case EXPR_UNARY:
            return node->as.op.op == OP_NOT ? PREC_NOT : PREC_NEGATE;
        case EXPR_BINARY:
            switch (node->as.op.op) {
                case OP_OR:            return PREC_OR;
                case OP_AND:           return PREC_AND;
                case OP_EQUAL:
                case OP_NOT_EQUAL:
                case OP_LESS:
                case OP_LESS_EQUAL:
                case OP_GREATER:
                case OP_GREATER_EQUAL: return PREC_COMPARE;
                case OP_ADD:
                case OP_SUBTRACT:      return PREC_ADD;
                case OP_MULTIPLY:
                case OP_DIVIDE:
                case OP_REMAINDER:     return PREC_MULTIPLY;
                case OP_NEGATE:
                case OP_NOT:           break;  // Only unary.
            }
    }
    return PREC_PRIMARY;
}

static void write_literal(Writer *w, const Literal *lit) {
    switch (lit->kind) {
        case LITERAL_INT:
            write_text(w, "%lld", lit->as.integer);
            break;
        case LITERAL_TEXT:
            // Quoted again, with each ' doubled, as it was written.
            write_text(w, "'");
            for (const char *c = lit->as.text; *c != '\0'; c++) {
                if (*c == '\'') {
                    write_text(w, "''");
                } else {
                    write_text(w, "%c", *c);
                }
            }
            write_text(w, "'");
            break;
        case LITERAL_BOOL:
            write_text(w, lit->as.boolean ? "TRUE" : "FALSE");
            break;
        case LITERAL_NULL:
            write_text(w, "NULL");
            break;
        case LITERAL_PARAM:
            write_text(w, "?");
            break;
    }
}

// Writes node, in brackets if it binds less tightly than outer needs.
static void write_node(Writer *w, const ExprNode *nodes, int index,
                       int outer) {
    const ExprNode *node = &nodes[index];
    int prec = precedence(node);
    int bracket = prec < outer;

    if (bracket) {
        write_text(w, "(");
    }

    switch (node->kind) {
        case EXPR_LITERAL:
            write_literal(w, &node->as.literal);
            break;
        case EXPR_COLUMN:
            write_text(w, "%s", node->as.column);
            break;
        case EXPR_UNARY:
            if (node->as.op.op == OP_NOT) {
                write_text(w, "NOT ");
                write_node(w, nodes, node->as.op.left, PREC_NOT);
            } else {
                // A primary only, so -(-1) can't become --1, a comment.
                write_text(w, "-");
                write_node(w, nodes, node->as.op.left, PREC_PRIMARY);
            }
            break;
        case EXPR_BINARY: {
            // The right side, and both sides of a comparison, which
            // can't be chained, need brackets at the same level:
            // `a - (b - c)`, `(a = b) = c`.
            int left = prec == PREC_COMPARE ? prec + 1 : prec;

            write_node(w, nodes, node->as.op.left, left);
            write_text(w, " %s ", operator_name(node->as.op.op));
            write_node(w, nodes, node->as.op.right, prec + 1);
            break;
        }
        case EXPR_IS_NULL:
            write_node(w, nodes, node->as.is_null.operand, PREC_COMPARE + 1);
            write_text(w, node->as.is_null.negated ? " IS NOT NULL"
                                                   : " IS NULL");
            break;
        case EXPR_AGGREGATE:
            write_text(w, "%s(", aggregate_name(node->as.aggregate.func));
            if (node->as.aggregate.operand < 0) {
                write_text(w, "*");
            } else {
                write_node(w, nodes, node->as.aggregate.operand, 0);
            }
            write_text(w, ")");
            break;
    }

    if (bracket) {
        write_text(w, ")");
    }
}

void expr_to_text(const ExprNode *nodes, int node, char *out, size_t size) {
    Writer w = { out, size, 0 };

    if (size > 0) {
        out[0] = '\0';
    }
    write_node(&w, nodes, node, 0);
}
