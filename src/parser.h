#ifndef TINYSQL_PARSER_H
#define TINYSQL_PARSER_H

#include "tokeniser.h"

#define MAX_COLUMNS 32

typedef enum {
    TYPE_INT,
    TYPE_TEXT,
    TYPE_NULL  // Only a value can be NULL; a column is INT or TEXT.
} DataType;

// A column in CREATE TABLE: `name TEXT`.
typedef struct {
    char name[MAX_TOKEN_LENGTH];
    DataType type;
} ColumnDef;

// A value written in the SQL: `25`, `'Alice'` or `NULL`. For NULL, as
// is unused.
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
    CMP_GREATER_EQUAL,
    CMP_IS_NULL,
    CMP_IS_NOT_NULL
} CompareOp;

// A column compared with a value: `age > 18`, or tested for NULL:
// `age IS NULL`, for which value is unused. A comparison's value is
// never NULL; the parser points the user to IS NULL instead.
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
// index into WhereClause.conditions rather than by pointer, so the
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

// The condition after WHERE, shared by SELECT and DELETE.
typedef struct {
    Condition conditions[MAX_CONDITIONS];
    int condition_count;
    int root;  // Index of the node for the whole condition.
} WhereClause;

typedef enum {
    AGG_COUNT,
    AGG_SUM,
    AGG_MIN,
    AGG_MAX
} AggregateFunc;

// A function over all the matching rows: `COUNT(*)` or `SUM(price)`.
typedef struct {
    AggregateFunc func;
    char column[MAX_TOKEN_LENGTH];  // Empty for COUNT(*).
} Aggregate;

typedef struct {
    // Exactly one of these lists is used. There's no GROUP BY, so a
    // SELECT can't mix columns with aggregates.
    int select_all;      // 1 for SELECT *.
    int has_aggregates;  // 1 for aggregates, e.g. SELECT COUNT(*), ...
    char columns[MAX_COLUMNS][MAX_TOKEN_LENGTH];
    int column_count;
    Aggregate aggregates[MAX_COLUMNS];
    int aggregate_count;

    int has_where;   // 1 if there's a WHERE, otherwise where is unused.
    WhereClause where;

    int has_order;   // 1 if there's an ORDER BY, otherwise rows keep
                     // the order they were inserted in.
    char order_column[MAX_TOKEN_LENGTH];
    int descending;  // 1 for DESC; ASC is the default.

    int has_limit;   // 1 if there's a LIMIT, otherwise every row.
    long long limit; // Never negative.
} SelectStatement;

typedef struct {
    int has_where;   // 0 deletes every row.
    WhereClause where;
} DeleteStatement;

// A column given a new value in UPDATE: `age = 26`.
typedef struct {
    char column[MAX_TOKEN_LENGTH];
    Literal value;
} Assignment;

typedef struct {
    Assignment assignments[MAX_COLUMNS];
    int assignment_count;

    int has_where;   // 0 updates every row.
    WhereClause where;
} UpdateStatement;

typedef enum {
    STMT_CREATE,
    STMT_INSERT,
    STMT_SELECT,
    STMT_DELETE,
    STMT_UPDATE,
    STMT_DROP        // Needs only table_name.
} StatementType;

// What the parser produces and the executor consumes.
typedef struct {
    StatementType type;
    char table_name[MAX_TOKEN_LENGTH];
    union {
        CreateStatement create;
        InsertStatement insert;
        SelectStatement select;
        DeleteStatement delete;
        UpdateStatement update;
    } as;
} Statement;

// The type as written in SQL: "INT", "TEXT" or "NULL".
const char *data_type_name(DataType type);

// Read a Literal's value. The type must match: reading the wrong member
// is a programmer error and stops the program.
long long literal_as_int(const Literal *lit);
const char *literal_as_text(const Literal *lit);

// Parses one statement from tokens. On success, fills out and returns 1.
// On failure, writes a message to error and returns 0; out must not be used.
int parse(const TokenList *tokens, Statement *out,
          char *error, size_t error_size);

#endif
