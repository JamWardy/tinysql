#ifndef TINYSQL_PARSER_H
#define TINYSQL_PARSER_H

#include "tokeniser.h"

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

// A column compared with a value: `age > 18`.
typedef struct {
    char column[MAX_TOKEN_LENGTH];
    CompareOp op;
    Literal value;
} Comparison;

typedef enum {
    COND_COMPARE,
    COND_AND,
    COND_OR,
    COND_NOT
} ConditionType;

// One node of a WHERE condition tree. Nodes refer to their children by
// index into SelectStatement.conditions rather than by pointer, so the
// tree lives in a fixed-size array and there's nothing to free.
typedef struct {
    ConditionType type;
    union {
        Comparison compare;  // COND_COMPARE
        struct {
            int left;
            int right;       // Unused for COND_NOT.
        } children;          // COND_AND, COND_OR, COND_NOT
    } as;
} Condition;

// `a = 1 AND NOT b = 2` uses four nodes: two comparisons, NOT and AND.
#define MAX_CONDITIONS 64

typedef struct {
    int select_all;  // 1 for SELECT *, otherwise use columns.
    char columns[MAX_COLUMNS][MAX_TOKEN_LENGTH];
    int column_count;

    int has_where;   // 1 if there's a WHERE, otherwise the rest is unused.
    Condition conditions[MAX_CONDITIONS];
    int condition_count;
    int where_root;  // Index of the node for the whole condition.
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
