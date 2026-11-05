#ifndef TINYSQL_PARSER_INTERNAL_H
#define TINYSQL_PARSER_INTERNAL_H

// Shared by the parser's files and included nowhere else; the rest of TinySQL
// includes parser.h.

#include "parser.h"
#include "tokeniser.h"

#define MAX_ERROR 256

typedef struct {
    const TokenList *tokens;
    int pos;                  // The next token to read.
    int param_count;          // `?`s read so far.
    char error[MAX_ERROR];    // Set by the first check that fails.
    Statement *stmt;          // Output; its nodes hold every expression.
} Parser;

// Statements, one per file; parse() picks one by the first token. Each fills
// out, or sets p->error and returns 0.

// schema.c
int parse_create(Parser *p, Statement *out);
int parse_drop(Parser *p, Statement *out);

// insert.c
int parse_insert(Parser *p, Statement *out);

// select.c
int parse_select(Parser *p, Statement *out);

// `*`, or items with AS. INSERT's RETURNING uses it too.
int parse_select_list(Parser *p, SelectList *out);

// update.c
int parse_update(Parser *p, Statement *out);

// delete.c
int parse_delete(Parser *p, Statement *out);

// transaction.c: BEGIN, COMMIT and ROLLBACK.
int parse_transaction(Parser *p, Statement *out);

// analyze.c
int parse_analyze(Parser *p, Statement *out);

// common.c: reading tokens and values.

const Token *peek(const Parser *p);

// Records "expected <what> but found ..." for the current token.
void error_expected(Parser *p, const char *what);

// Consumes the token if it has the given type. Otherwise records what was
// expected and returns NULL.
const Token *expect(Parser *p, TokenType type, const char *what);

// Consumes the token if it has the given type. A mismatch isn't an error: it's
// for where the grammar has a choice.
int match(Parser *p, TokenType type);

// Only the statement knows what else could have come next, e.g. "AND, OR or
// ';'" after a condition, so it names them in the message.
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

// Fails if the statement has MAX_NODES already.
int new_node(Parser *p, ExprKind kind, int *out);

// Sets *out to the root node.
int parse_expr(Parser *p, int *out);

// [ WHERE expr ], in SELECT, UPDATE and DELETE.
int parse_where(Parser *p, int *has_where, int *where);

#endif
