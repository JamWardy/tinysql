#ifndef TINYSQL_PARSER_INTERNAL_H
#define TINYSQL_PARSER_INTERNAL_H

// Shared between the parser's files in this folder, and not for use
// outside it; the rest of TinySQL includes parser.h. Each statement
// has its own file, and calls into common.c and where.c.

#include "parser.h"
#include "tokeniser.h"

#define MAX_ERROR 256

// The parser's working state while it reads one statement. It lives only
// for the duration of a parse; the result goes into the caller's Statement.
typedef struct {
    const TokenList *tokens;  // Input, read-only.
    int pos;                  // Index of the next token to read.
    int param_count;          // `?`s read so far.
    char error[MAX_ERROR];    // Set by the first check that fails.
    Statement *stmt;          // Output; its nodes hold every expression.
} Parser;

// Statements, one per file: parse() in parser.c picks the one for the
// first token. Each fills out and returns 1, or returns 0 with a
// message in p->error.

// schema.c
int parse_create(Parser *p, Statement *out);
int parse_drop(Parser *p, Statement *out);

// insert.c
int parse_insert(Parser *p, Statement *out);

// select.c
int parse_select(Parser *p, Statement *out);

// update.c
int parse_update(Parser *p, Statement *out);

// delete.c
int parse_delete(Parser *p, Statement *out);

// transaction.c: BEGIN, COMMIT and ROLLBACK.
int parse_transaction(Parser *p, Statement *out);

// common.c: reading tokens and values.

// Returns the current token without consuming it.
const Token *peek(const Parser *p);

// Records "expected <what> but found ..." for the current token.
void error_expected(Parser *p, const char *what);

// Consumes the current token if it has the given type and returns it.
// Otherwise records an error naming what was expected and returns NULL.
const Token *expect(Parser *p, TokenType type, const char *what);

// Consumes the current token if it has the given type. Unlike expect,
// a mismatch isn't an error: it's used where the grammar has a choice.
int match(Parser *p, TokenType type);

// Checks that the statement ends here. parse would also catch a missing
// ';', but only the statement knows what else could have come next, e.g.
// "AND, OR or ';'" after a condition, so it names those options.
int expect_end(Parser *p, const char *options);

// The only places a Literal is built, so its tag always matches its value.
Literal literal_int(long long n);
Literal literal_text(const char *text);
Literal literal_bool(int b);
Literal literal_null(void);
Literal literal_param(Parser *p);  // The next `?` in p's statement.

// value = [ MINUS ] NUMBER | STRING | TRUE | FALSE | NULL | PARAM
int parse_value(Parser *p, Literal *out);

// expr.c: expressions, built into p->stmt->nodes.

// Adds a node of the given kind to the statement and sets *out to its
// index. Returns 1, or 0 if the statement has MAX_NODES already.
int new_node(Parser *p, ExprKind kind, int *out);

// Parses an expression and sets *out to the index of its root node.
int parse_expr(Parser *p, int *out);

// [ WHERE expr ], in SELECT, UPDATE and DELETE. Sets *has_where to
// whether there was one, and *where to its root node.
int parse_where(Parser *p, int *has_where, int *where);

#endif
