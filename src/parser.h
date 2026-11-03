#ifndef TINYSQL_PARSER_H
#define TINYSQL_PARSER_H

#include "tokeniser.h"

#define MAX_COLUMNS 32
#define MAX_KEYS 8  // Per table, the PRIMARY KEY included.

typedef enum {
    TYPE_INT,
    TYPE_TEXT,
    TYPE_BOOL,  // Only an expression can be a BOOL, not a column.
    TYPE_NULL   // Only a value can be NULL; a column is INT or TEXT.
} DataType;

typedef enum {
    CONSTRAINT_PRIMARY_KEY,
    CONSTRAINT_UNIQUE
} ConstraintKind;

// A column in CREATE TABLE: `name TEXT` or `id INT NOT NULL`.
typedef struct {
    char name[MAX_TOKEN_LENGTH];
    DataType type;
    int not_null;  // 1 for NOT NULL.
} ColumnDef;

typedef enum {
    LITERAL_INT,
    LITERAL_TEXT,
    LITERAL_BOOL,
    LITERAL_NULL,
    LITERAL_PARAM  // A `?`, whose value is passed separately.
} LiteralKind;

// A value written in the SQL: `25`, `'Alice'`, `TRUE`, `NULL` or `?`.
// kind says which member of as holds it; NULL has none.
typedef struct {
    LiteralKind kind;
    union {
        long long integer;
        char text[MAX_TOKEN_LENGTH];
        int boolean;  // 1 for TRUE, 0 for FALSE.
        int param;    // Which `?`, from 0, left to right in the statement.
    } as;
} Literal;

typedef enum {
    AGG_COUNT,
    AGG_SUM,
    AGG_MIN,
    AGG_MAX
} AggregateFunc;

typedef enum {
    OP_NEGATE,     // `-a`
    OP_NOT,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_REMAINDER,  // `%`
    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,
    OP_AND,
    OP_OR
} Operator;

typedef enum {
    EXPR_LITERAL,
    EXPR_COLUMN,
    EXPR_UNARY,      // OP_NEGATE or OP_NOT, on left.
    EXPR_BINARY,     // Any other operator, on left and right.
    EXPR_IS_NULL,    // `a IS NULL` or `a IS NOT NULL`, on left.
    EXPR_AGGREGATE   // `COUNT(*)`, or a function of left.
} ExprKind;

// One node of an expression tree. Nodes refer to their children by
// index into Statement.nodes rather than by pointer, so every tree in a
// statement lives in one fixed-size array and there's nothing to free.
typedef struct {
    ExprKind kind;
    union {
        Literal literal;                // EXPR_LITERAL
        char column[MAX_TOKEN_LENGTH];  // EXPR_COLUMN
        struct {
            Operator op;
            int left;
            int right;                  // EXPR_BINARY only.
        } op;                           // EXPR_UNARY, EXPR_BINARY
        struct {
            int operand;
            int negated;                // 1 for IS NOT NULL.
        } is_null;                      // EXPR_IS_NULL
        struct {
            AggregateFunc func;
            int operand;                // -1 for COUNT(*).
        } aggregate;                    // EXPR_AGGREGATE
    } as;
} ExprNode;

// Each node takes at least one token, so a statement can't need more.
#define MAX_NODES MAX_TOKENS

// A key in CREATE TABLE: `id INT PRIMARY KEY` or `UNIQUE (user_id,
// idempotency_key)`. The executor checks the names are columns.
typedef struct {
    ConstraintKind kind;
    char columns[MAX_COLUMNS][MAX_TOKEN_LENGTH];  // In key order.
    int column_count;
} KeyDef;

typedef struct {
    ColumnDef columns[MAX_COLUMNS];
    int column_count;

    KeyDef keys[MAX_KEYS];  // In the order written.
    int key_count;
} CreateStatement;

typedef struct {
    // `INSERT INTO t (a, b)`: the columns the values go in, in order.
    // 0 when there's no list, so the values fill every column in order.
    char columns[MAX_COLUMNS][MAX_TOKEN_LENGTH];
    int column_count;

    int values[MAX_COLUMNS];  // Each the root node of an expression.
    int value_count;
} InsertStatement;

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
    int where;       // The root node of a BOOL expression.

    int has_order;   // 1 if there's an ORDER BY, otherwise rows keep
                     // the order they were inserted in.
    char order_column[MAX_TOKEN_LENGTH];
    int descending;  // 1 for DESC; ASC is the default.

    int has_limit;   // 1 if there's a LIMIT, otherwise every row.
    Literal limit;   // An INT, never negative, or a `?`.
} SelectStatement;

typedef struct {
    int has_where;   // 0 deletes every row.
    int where;       // The root node of a BOOL expression.
} DeleteStatement;

// A column given a new value in UPDATE: `age = 26` or `stock = stock - ?`.
typedef struct {
    char column[MAX_TOKEN_LENGTH];
    int value;  // The root node of an expression.
} Assignment;

typedef struct {
    Assignment assignments[MAX_COLUMNS];
    int assignment_count;

    int has_where;   // 0 updates every row.
    int where;       // The root node of a BOOL expression.
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
    int param_count;  // How many `?`s, each a LITERAL_PARAM.

    ExprNode nodes[MAX_NODES];  // Every expression in the statement.
    int node_count;

    union {
        CreateStatement create;
        InsertStatement insert;
        SelectStatement select;
        DeleteStatement delete;
        UpdateStatement update;
    } as;
} Statement;

// The type as written in SQL: "INT", "TEXT", "BOOL" or "NULL".
const char *data_type_name(DataType type);

// The operator as written in SQL, e.g. "+", "<=" or "AND".
const char *operator_name(Operator op);

// The function as written in SQL, e.g. "COUNT".
const char *aggregate_name(AggregateFunc func);

// The constraint as written in SQL: "PRIMARY KEY" or "UNIQUE".
const char *constraint_kind_name(ConstraintKind kind);

// Read a Literal's value. The kind must match: reading the wrong member
// is a programmer error and stops the program.
long long literal_as_int(const Literal *lit);
const char *literal_as_text(const Literal *lit);

// Writes node's expression into out as SQL, rebuilt from the tree with
// brackets only where they're needed, e.g. "price * quantity" or
// "(a + b) * 2". Cut short to fit size bytes.
void expr_to_text(const ExprNode *nodes, int node, char *out, size_t size);

// Parses one statement from tokens. On success, fills out and returns 1.
// On failure, writes a message to error and returns 0; out must not be used.
int parse(const TokenList *tokens, Statement *out,
          char *error, size_t error_size);

#endif
