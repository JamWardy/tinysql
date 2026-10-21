#ifndef TINYSQL_PARSER_H
#define TINYSQL_PARSER_H

#include "tokenizer.h"

#define MAX_COLUMNS 32

typedef enum {
    TYPE_INT,
    TYPE_TEXT
} DataType;

// A column in CREATE TABLE: `name TEXT`.
typedef struct {
    char name[MAX_TOKEN_LENGTH];
    DataType type;
} ColumnDef;

// A value written in the SQL: `25` or `'Alice'`.
typedef struct {
    DataType type;
    union {
        long long integer;
        char text[MAX_TOKEN_LENGTH];
    } as;
} Literal;

typedef struct {
    ColumnDef columns[MAX_COLUMNS];
    int column_count;
} CreateStatement;

typedef struct {
    Literal values[MAX_COLUMNS];
    int value_count;
} InsertStatement;

typedef enum {
    CMP_EQUAL,
    CMP_NOT_EQUAL,
    CMP_LESS,
    CMP_LESS_EQUAL,
    CMP_GREATER,
    CMP_GREATER_EQUAL
} CompareOp;

// A WHERE condition: `age > 18`.
typedef struct {
    char column[MAX_TOKEN_LENGTH];
    CompareOp op;
    Literal value;
} Condition;

typedef struct {
    int select_all;  // 1 for SELECT *, otherwise use columns.
    char columns[MAX_COLUMNS][MAX_TOKEN_LENGTH];
    int column_count;
    int has_where;   // 1 if there's a WHERE, otherwise where is unused.
    Condition where;
} SelectStatement;

typedef enum {
    STMT_CREATE,
    STMT_INSERT,
    STMT_SELECT
} StatementType;

// What the parser produces and the executor consumes.
typedef struct {
    StatementType type;
    char table_name[MAX_TOKEN_LENGTH];
    union {
        CreateStatement create;
        InsertStatement insert;
        SelectStatement select;
    } as;
} Statement;

// Read a Literal's value. The type must match: reading the wrong member
// is a programmer error and stops the program.
long long literal_as_int(const Literal *lit);
const char *literal_as_text(const Literal *lit);

// Parses one statement from tokens. On success, fills out and returns 1.
// On failure, writes a message to error and returns 0; out must not be used.
int parse(const TokenList *tokens, Statement *out,
          char *error, size_t error_size);

#endif
