#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "text.h"

const Token *peek(const Parser *p) {
    // The tokeniser guarantees the list ends in EOF, and nothing consumes
    // EOF, so pos can only run past the end if the parser has a bug.
    assert(p->pos < p->tokens->count);
    return &p->tokens->tokens[p->pos];
}

void error_expected(Parser *p, const char *what) {
    const Token *token = peek(p);

    if (token->type == TOKEN_EOF) {
        fail(p->error, sizeof(p->error),
             "expected %s but reached end of input", what);
    } else {
        fail(p->error, sizeof(p->error),
             "expected %s but found '%s'", what, token->text);
    }
}

const Token *expect(Parser *p, TokenType type, const char *what) {
    const Token *token = peek(p);

    if (token->type != type) {
        error_expected(p, what);
        return NULL;
    }

    p->pos++;
    return token;
}

int match(Parser *p, TokenType type) {
    if (peek(p)->type != type) {
        return 0;
    }

    p->pos++;
    return 1;
}

Literal literal_int(long long n) {
    Literal lit;
    lit.kind = LITERAL_INT;
    lit.as.integer = n;
    return lit;
}

Literal literal_text(const char *text) {
    Literal lit;
    lit.kind = LITERAL_TEXT;
    copy_text(lit.as.text, sizeof(lit.as.text), text);
    return lit;
}

Literal literal_bool(int b) {
    Literal lit;
    lit.kind = LITERAL_BOOL;
    lit.as.boolean = b;
    return lit;
}

Literal literal_null(void) {
    Literal lit;
    lit.kind = LITERAL_NULL;
    return lit;
}

Literal literal_param(Parser *p) {
    Literal lit;
    lit.kind = LITERAL_PARAM;
    lit.as.param = p->param_count++;
    return lit;
}

const char *data_type_name(DataType type) {
    switch (type) {
        case TYPE_INT:  return "INT";
        case TYPE_TEXT: return "TEXT";
        case TYPE_BOOL: return "BOOL";
        case TYPE_NULL: return "NULL";
    }
    return "UNKNOWN";
}

long long literal_as_int(const Literal *lit) {
    assert(lit->kind == LITERAL_INT);
    return lit->as.integer;
}

const char *literal_as_text(const Literal *lit) {
    assert(lit->kind == LITERAL_TEXT);
    return lit->as.text;
}

const char *operator_name(Operator op) {
    switch (op) {
        case OP_NEGATE:        return "-";
        case OP_NOT:           return "NOT";
        case OP_ADD:           return "+";
        case OP_SUBTRACT:      return "-";
        case OP_MULTIPLY:      return "*";
        case OP_DIVIDE:        return "/";
        case OP_REMAINDER:     return "%";
        case OP_EQUAL:         return "=";
        case OP_NOT_EQUAL:     return "!=";
        case OP_LESS:          return "<";
        case OP_LESS_EQUAL:    return "<=";
        case OP_GREATER:       return ">";
        case OP_GREATER_EQUAL: return ">=";
        case OP_AND:           return "AND";
        case OP_OR:            return "OR";
    }
    return "?";
}

const char *aggregate_name(AggregateFunc func) {
    switch (func) {
        case AGG_COUNT: return "COUNT";
        case AGG_SUM:   return "SUM";
        case AGG_MIN:   return "MIN";
        case AGG_MAX:   return "MAX";
    }
    return "?";
}

const char *constraint_kind_name(ConstraintKind kind) {
    switch (kind) {
        case CONSTRAINT_PRIMARY_KEY: return "PRIMARY KEY";
        case CONSTRAINT_UNIQUE:      return "UNIQUE";
    }
    return "UNKNOWN";
}

int parse_value(Parser *p, Literal *out) {
    int negative = match(p, TOKEN_MINUS);
    const Token *token = peek(p);

    if (token->type == TOKEN_NUMBER) {
        // Convert the sign and digits together rather than negating
        // afterwards: the most negative long long has no positive
        // counterpart, so it can't be read as positive first.
        char text[MAX_TOKEN_LENGTH + 1];
        snprintf(text, sizeof(text), "%s%s", negative ? "-" : "",
                 token->text);

        // The tokeniser only accepts digits, so the whole text converts;
        // the remaining failure is a number too big for a long long.
        errno = 0;
        long long n = strtoll(text, NULL, 10);

        if (errno == ERANGE) {
            return fail(p->error, sizeof(p->error),
                        "number out of range: %s", text);
        }

        *out = literal_int(n);
    } else if (token->type == TOKEN_STRING && !negative) {
        *out = literal_text(token->text);
    } else if ((token->type == TOKEN_TRUE || token->type == TOKEN_FALSE) &&
               !negative) {
        *out = literal_bool(token->type == TOKEN_TRUE);
    } else if (token->type == TOKEN_NULL && !negative) {
        *out = literal_null();
    } else if (token->type == TOKEN_PARAM && !negative) {
        *out = literal_param(p);
    } else {
        error_expected(p, negative ? "number after '-'"
                                   : "value (number, 'text', NULL or ?)");
        return 0;
    }

    p->pos++;
    return 1;
}

int expect_end(Parser *p, const char *options) {
    if (peek(p)->type != TOKEN_SEMICOLON) {
        error_expected(p, options);
        return 0;
    }

    return 1;
}
