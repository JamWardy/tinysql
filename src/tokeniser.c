#include "tokeniser.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOKEN_IDENTIFIER:     return "IDENTIFIER";
        case TOKEN_NUMBER:         return "NUMBER";
        case TOKEN_STRING:         return "STRING";
        case TOKEN_LPAREN:         return "LPAREN";
        case TOKEN_RPAREN:         return "RPAREN";
        case TOKEN_COMMA:          return "COMMA";
        case TOKEN_SEMICOLON:      return "SEMICOLON";
        case TOKEN_STAR:           return "STAR";
        case TOKEN_MINUS:          return "MINUS";
        case TOKEN_EQUAL:          return "EQUAL";
        case TOKEN_NOT_EQUAL:      return "NOT_EQUAL";
        case TOKEN_LESS:           return "LESS";
        case TOKEN_LESS_EQUAL:     return "LESS_EQUAL";
        case TOKEN_GREATER:        return "GREATER";
        case TOKEN_GREATER_EQUAL:  return "GREATER_EQUAL";
        case TOKEN_CREATE:         return "CREATE";
        case TOKEN_TABLE:          return "TABLE";
        case TOKEN_INSERT:         return "INSERT";
        case TOKEN_INTO:           return "INTO";
        case TOKEN_VALUES:         return "VALUES";
        case TOKEN_SELECT:         return "SELECT";
        case TOKEN_FROM:           return "FROM";
        case TOKEN_WHERE:          return "WHERE";
        case TOKEN_INT:            return "INT";
        case TOKEN_TEXT:           return "TEXT";
        case TOKEN_EOF:            return "EOF";
    }
    return "UNKNOWN";
}

typedef struct {
    const char *text;
    TokenType type;
} Keyword;

static const Keyword keywords[] = {
    { "CREATE", TOKEN_CREATE },
    { "TABLE",  TOKEN_TABLE },
    { "INSERT", TOKEN_INSERT },
    { "INTO",   TOKEN_INTO },
    { "VALUES", TOKEN_VALUES },
    { "SELECT", TOKEN_SELECT },
    { "FROM",   TOKEN_FROM },
    { "WHERE",  TOKEN_WHERE },
    { "INT",    TOKEN_INT },
    { "TEXT",   TOKEN_TEXT },
};

// Compares a word of the given length against an upper-case keyword,
// ignoring case. Both must have the same length to match.
static int keyword_equals(const char *word, int length, const char *keyword) {
    for (int i = 0; i < length; i++) {
        if (keyword[i] == '\0' ||
            toupper((unsigned char)word[i]) != keyword[i]) {
            return 0;
        }
    }

    return keyword[length] == '\0';
}

static TokenType lookup_keyword(const char *word, int length) {
    int count = (int)(sizeof(keywords) / sizeof(keywords[0]));

    for (int i = 0; i < count; i++) {
        if (keyword_equals(word, length, keywords[i].text)) {
            return keywords[i].type;
        }
    }

    return TOKEN_IDENTIFIER;
}

static int is_identifier_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int is_identifier_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// Appends a token, or writes an error and returns 0 if it doesn't fit.
static int add_token(
    TokenList *list,
    TokenType type,
    const char *start,
    int length,
    char *error,
    size_t error_size
) {
    // Keep the last slot free so there's always room for EOF.
    if (type != TOKEN_EOF && list->count >= MAX_TOKENS - 1) {
        snprintf(error, error_size,
                 "too many tokens (max %d)", MAX_TOKENS - 1);
        return 0;
    }

    if (length >= MAX_TOKEN_LENGTH) {
        snprintf(error, error_size,
                 "token too long (max %d characters): '%.20s...'",
                 MAX_TOKEN_LENGTH - 1, start);
        return 0;
    }

    Token *token = &list->tokens[list->count++];

    token->type = type;
    memcpy(token->text, start, (size_t)length);
    token->text[length] = '\0';
    return 1;
}

int tokenise(const char *sql, TokenList *list, char *error, size_t error_size) {
    int i = 0;

    list->count = 0;

    while (sql[i] != '\0') {
        unsigned char c = (unsigned char)sql[i];

        if (isspace(c)) {
            i++;
            continue;
        }

        if (is_identifier_start((char)c)) {
            int start = i++;

            while (is_identifier_char(sql[i])) {
                i++;
            }

            TokenType type = lookup_keyword(sql + start, i - start);
            if (!add_token(list, type, sql + start, i - start,
                           error, error_size)) {
                return 0;
            }
            continue;
        }

        if (isdigit(c)) {
            int start = i++;

            while (isdigit((unsigned char)sql[i])) {
                i++;
            }

            if (!add_token(list, TOKEN_NUMBER, sql + start, i - start,
                           error, error_size)) {
                return 0;
            }
            continue;
        }

        if (sql[i] == '\'') {
            int start = ++i;

            while (sql[i] != '\0' && sql[i] != '\'') {
                i++;
            }

            if (sql[i] == '\0') {
                snprintf(error, error_size, "unterminated string");
                return 0;
            }

            if (!add_token(list, TOKEN_STRING, sql + start, i - start,
                           error, error_size)) {
                return 0;
            }
            i++;  // Skip closing quote.
            continue;
        }

        TokenType type;
        int length = 2;

        // Two-character operators first, so "<=" isn't read as "<" "=".
        if (strncmp(sql + i, "<=", 2) == 0) {
            type = TOKEN_LESS_EQUAL;
        } else if (strncmp(sql + i, ">=", 2) == 0) {
            type = TOKEN_GREATER_EQUAL;
        } else if (strncmp(sql + i, "!=", 2) == 0 ||
                   strncmp(sql + i, "<>", 2) == 0) {
            type = TOKEN_NOT_EQUAL;
        } else {
            length = 1;

            switch (sql[i]) {
                case '(': type = TOKEN_LPAREN; break;
                case ')': type = TOKEN_RPAREN; break;
                case ',': type = TOKEN_COMMA; break;
                case ';': type = TOKEN_SEMICOLON; break;
                case '*': type = TOKEN_STAR; break;
                case '-': type = TOKEN_MINUS; break;
                case '=': type = TOKEN_EQUAL; break;
                case '<': type = TOKEN_LESS; break;
                case '>': type = TOKEN_GREATER; break;

                default:
                    snprintf(error, error_size,
                             "unexpected character '%c'", sql[i]);
                    return 0;
            }
        }

        if (!add_token(list, type, sql + i, length, error, error_size)) {
            return 0;
        }
        i += length;
    }

    // Can't fail: add_token always leaves room for EOF.
    add_token(list, TOKEN_EOF, "", 0, error, error_size);
    return 1;
}
