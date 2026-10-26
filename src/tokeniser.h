#ifndef TINYSQL_TOKENISER_H
#define TINYSQL_TOKENISER_H

#include <stddef.h>

#define MAX_TOKENS 256
#define MAX_TOKEN_LENGTH 128

typedef enum {
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_COMMA,
    TOKEN_SEMICOLON,
    TOKEN_STAR,
    TOKEN_MINUS,
    TOKEN_EQUAL,
    TOKEN_NOT_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,
    TOKEN_CREATE,
    TOKEN_TABLE,
    TOKEN_INSERT,
    TOKEN_INTO,
    TOKEN_VALUES,
    TOKEN_SELECT,
    TOKEN_FROM,
    TOKEN_DELETE,
    TOKEN_UPDATE,
    TOKEN_SET,
    TOKEN_DROP,
    TOKEN_ORDER,
    TOKEN_BY,
    TOKEN_ASC,
    TOKEN_DESC,
    TOKEN_LIMIT,
    TOKEN_WHERE,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,
    TOKEN_INT,
    TOKEN_TEXT,
    TOKEN_EOF
} TokenType;

typedef struct {
    TokenType type;
    char text[MAX_TOKEN_LENGTH];
} Token;

typedef struct {
    Token tokens[MAX_TOKENS];
    int count;
} TokenList;

// Splits sql into tokens. On success, fills list (always ending in
// TOKEN_EOF) and returns 1. On failure, writes a message to error and
// returns 0; list must not be used.
int tokenise(const char *sql, TokenList *list, char *error, size_t error_size);
const char *token_type_name(TokenType type);

#endif
