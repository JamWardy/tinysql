#include "parser.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_ERROR 256

// The parser's working state while it reads one statement. It lives only
// for the duration of a parse; the result goes into the caller's Statement.
typedef struct {
    const TokenList *tokens;  // Input, read-only.
    int pos;                  // Index of the next token to read.
    char error[MAX_ERROR];    // Set by the first check that fails.
} Parser;

// Returns the current token without consuming it.
static const Token *peek(const Parser *p) {
    // The tokenizer guarantees the list ends in EOF, and nothing consumes
    // EOF, so pos can only run past the end if the parser has a bug.
    assert(p->pos < p->tokens->count);
    return &p->tokens->tokens[p->pos];
}

// Records "expected <what> but found ..." for the current token.
static void error_expected(Parser *p, const char *what) {
    const Token *token = peek(p);

    if (token->type == TOKEN_EOF) {
        snprintf(p->error, sizeof(p->error),
                 "expected %s but reached end of input", what);
    } else {
        snprintf(p->error, sizeof(p->error),
                 "expected %s but found '%s'", what, token->text);
    }
}

// Consumes the current token if it has the given type and returns it.
// Otherwise records an error naming what was expected and returns NULL.
static const Token *expect(Parser *p, TokenType type, const char *what) {
    const Token *token = peek(p);

    if (token->type != type) {
        error_expected(p, what);
        return NULL;
    }

    p->pos++;
    return token;
}

// Consumes the current token if it has the given type. Unlike expect,
// a mismatch isn't an error: it's used where the grammar has a choice.
static int match(Parser *p, TokenType type) {
    if (peek(p)->type != type) {
        return 0;
    }

    p->pos++;
    return 1;
}

// The only places a Literal is built, so its tag always matches its value.
static Literal literal_int(long long n) {
    Literal lit;
    lit.type = TYPE_INT;
    lit.as.integer = n;
    return lit;
}

static Literal literal_text(const char *text) {
    Literal lit;
    lit.type = TYPE_TEXT;
    snprintf(lit.as.text, sizeof(lit.as.text), "%s", text);
    return lit;
}

long long literal_as_int(const Literal *lit) {
    assert(lit->type == TYPE_INT);
    return lit->as.integer;
}

const char *literal_as_text(const Literal *lit) {
    assert(lit->type == TYPE_TEXT);
    return lit->as.text;
}

// column_def = IDENTIFIER column_type
// column_type = INT | TEXT
static int parse_column_def(Parser *p, ColumnDef *out) {
    const Token *name = expect(p, TOKEN_IDENTIFIER, "column name");
    if (!name) {
        return 0;
    }

    if (match(p, TOKEN_INT)) {
        out->type = TYPE_INT;
    } else if (match(p, TOKEN_TEXT)) {
        out->type = TYPE_TEXT;
    } else {
        error_expected(p, "column type (INT or TEXT)");
        return 0;
    }

    snprintf(out->name, sizeof(out->name), "%s", name->text);
    return 1;
}

// create_stmt = CREATE TABLE IDENTIFIER
//               LPAREN column_def { COMMA column_def } RPAREN
static int parse_create(Parser *p, Statement *out) {
    if (!expect(p, TOKEN_CREATE, "CREATE") ||
        !expect(p, TOKEN_TABLE, "TABLE")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    if (!expect(p, TOKEN_LPAREN, "'('")) {
        return 0;
    }

    CreateStatement *create = &out->as.create;
    create->column_count = 0;

    do {
        if (create->column_count == MAX_COLUMNS) {
            snprintf(p->error, sizeof(p->error),
                     "too many columns (max %d)", MAX_COLUMNS);
            return 0;
        }

        if (!parse_column_def(p, &create->columns[create->column_count])) {
            return 0;
        }

        create->column_count++;
    } while (match(p, TOKEN_COMMA));

    if (!expect(p, TOKEN_RPAREN, "',' or ')'")) {
        return 0;
    }

    out->type = STMT_CREATE;
    snprintf(out->table_name, sizeof(out->table_name), "%s", name->text);
    return 1;
}

// value = NUMBER | STRING
static int parse_value(Parser *p, Literal *out) {
    const Token *token = peek(p);

    if (token->type == TOKEN_NUMBER) {
        // The tokenizer only accepts digits, so the whole text converts;
        // the remaining failure is a number too big for a long long.
        errno = 0;
        long long n = strtoll(token->text, NULL, 10);

        if (errno == ERANGE) {
            snprintf(p->error, sizeof(p->error),
                     "number out of range: %s", token->text);
            return 0;
        }

        *out = literal_int(n);
    } else if (token->type == TOKEN_STRING) {
        *out = literal_text(token->text);
    } else {
        error_expected(p, "value (number or 'text')");
        return 0;
    }

    p->pos++;
    return 1;
}

// insert_stmt = INSERT INTO IDENTIFIER
//               VALUES LPAREN value { COMMA value } RPAREN
static int parse_insert(Parser *p, Statement *out) {
    if (!expect(p, TOKEN_INSERT, "INSERT") ||
        !expect(p, TOKEN_INTO, "INTO")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    if (!expect(p, TOKEN_VALUES, "VALUES") ||
        !expect(p, TOKEN_LPAREN, "'('")) {
        return 0;
    }

    InsertStatement *insert = &out->as.insert;
    insert->value_count = 0;

    do {
        if (insert->value_count == MAX_COLUMNS) {
            snprintf(p->error, sizeof(p->error),
                     "too many values (max %d)", MAX_COLUMNS);
            return 0;
        }

        if (!parse_value(p, &insert->values[insert->value_count])) {
            return 0;
        }

        insert->value_count++;
    } while (match(p, TOKEN_COMMA));

    if (!expect(p, TOKEN_RPAREN, "',' or ')'")) {
        return 0;
    }

    out->type = STMT_INSERT;
    snprintf(out->table_name, sizeof(out->table_name), "%s", name->text);
    return 1;
}

// select_list = STAR | IDENTIFIER { COMMA IDENTIFIER }
static int parse_select_list(Parser *p, SelectStatement *out) {
    out->column_count = 0;

    if (match(p, TOKEN_STAR)) {
        out->select_all = 1;
        return 1;
    }

    out->select_all = 0;

    do {
        if (out->column_count == MAX_COLUMNS) {
            snprintf(p->error, sizeof(p->error),
                     "too many columns (max %d)", MAX_COLUMNS);
            return 0;
        }

        const Token *column = expect(
            p, TOKEN_IDENTIFIER,
            out->column_count == 0 ? "'*' or column name" : "column name");
        if (!column) {
            return 0;
        }

        snprintf(out->columns[out->column_count],
                 sizeof(out->columns[out->column_count]),
                 "%s", column->text);
        out->column_count++;
    } while (match(p, TOKEN_COMMA));

    return 1;
}

// select_stmt = SELECT select_list FROM IDENTIFIER
static int parse_select(Parser *p, Statement *out) {
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

    out->type = STMT_SELECT;
    snprintf(out->table_name, sizeof(out->table_name), "%s", name->text);
    return 1;
}

// statement = ( create_stmt | insert_stmt | select_stmt ) SEMICOLON EOF
int parse(const TokenList *tokens, Statement *out,
          char *error, size_t error_size) {
    Parser p = { tokens, 0, "" };
    int ok;

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

        default:
            error_expected(&p, "CREATE, INSERT or SELECT");
            ok = 0;
            break;
    }

    ok = ok &&
         expect(&p, TOKEN_SEMICOLON, "';'") != NULL &&
         expect(&p, TOKEN_EOF, "end of input") != NULL;

    if (!ok) {
        snprintf(error, error_size, "%s", p.error);
        return 0;
    }

    return 1;
}
