#ifndef TINYSQL_PARSER_H
#define TINYSQL_PARSER_H

#include "error.h"
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

typedef struct {
    char name[MAX_TOKEN_LENGTH];
    DataType type;
    int not_null;
} ColumnDef;

typedef enum {
    LITERAL_INT,
    LITERAL_TEXT,
    LITERAL_BOOL,
    LITERAL_NULL,
    LITERAL_PARAM  // A `?`, whose value is passed separately.
} LiteralKind;

// kind says which member of as holds the value; NULL has none.
typedef struct {
    LiteralKind kind;
    union {
        long long integer;
        char text[MAX_TOKEN_LENGTH];
        int boolean;
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
    EXPR_LIKE,       // `a [NOT] LIKE p [ESCAPE e]`, or ILIKE.
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
            int operand;
            int pattern;
            int escape;                 // -1 for the default, `\`.
            int negated;                // 1 for NOT LIKE.
            int ignore_case;            // 1 for ILIKE.
        } like;                         // EXPR_LIKE
        struct {
            AggregateFunc func;
            int operand;                // -1 for COUNT(*).
        } aggregate;                    // EXPR_AGGREGATE
    } as;
} ExprNode;

// Each node takes at least one token, so a statement can't need more.
#define MAX_NODES MAX_TOKENS

// From CREATE TABLE. The executor checks the names are columns.
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

    // 1 if a column says AUTOINCREMENT; the executor checks it's an INT PRIMARY
    // KEY.
    int has_autoincrement;
    int autoincrement_column;  // Its index in columns.
} CreateStatement;

// One item in a SELECT list: `price * quantity AS total`.
typedef struct {
    int expr;                      // The root node.
    char alias[MAX_TOKEN_LENGTH];  // From AS, or empty.
} SelectItem;

// What a SELECT, or an INSERT's RETURNING, gives back: `*`, or items.
typedef struct {
    int select_all;  // 1 for `*`, which has no items.
    SelectItem items[MAX_COLUMNS];
    int item_count;
} SelectList;

typedef struct {
    // `INSERT INTO t (a, b)`: the columns the values go in, in order.
    // 0 when there's no list, so the values fill every column in order.
    char columns[MAX_COLUMNS][MAX_TOKEN_LENGTH];
    int column_count;

    int values[MAX_COLUMNS];  // Root nodes.
    int value_count;

    int has_returning;
    SelectList returning;  // Read from the inserted row.
} InsertStatement;

typedef struct {
    SelectList list;

    int has_where;
    int where;       // The root node of a BOOL expression.

    int has_order;   // Without one, rows keep insertion order.
    int order_by;    // The root node.
    int descending;

    int has_limit;
    Literal limit;   // An INT, never negative, or a `?`.

    // 1 for FOR UPDATE, which locks nothing more yet: TinySQL runs one
    // transaction at a time.
    int for_update;
} SelectStatement;

// CREATE INDEX name ON table (a, b): the table is the Statement's
// table_name.
typedef struct {
    char name[MAX_TOKEN_LENGTH];
    char columns[MAX_COLUMNS][MAX_TOKEN_LENGTH];  // In key order.
    int column_count;
} CreateIndexStatement;

typedef struct {
    int has_where;   // 0 deletes every row.
    int where;       // The root node of a BOOL expression.
} DeleteStatement;

// A column given a new value in UPDATE: `age = 26` or `stock = stock - ?`.
typedef struct {
    char column[MAX_TOKEN_LENGTH];
    int value;  // The root node.
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
    STMT_DROP,       // Needs only table_name.
    STMT_CREATE_INDEX,
    STMT_DROP_INDEX, // Needs only index_name.
    STMT_BEGIN,      // These three need nothing more.
    STMT_COMMIT,
    STMT_ROLLBACK,
    STMT_ANALYZE     // table_name, or empty for every table.
} StatementType;

// What the parser produces and the executor consumes.
typedef struct {
    StatementType type;
    int explain;  // 1 for EXPLAIN: describe how it would find its rows
                  // instead of running it. Only on SELECT, UPDATE, DELETE.
    char table_name[MAX_TOKEN_LENGTH];
    char index_name[MAX_TOKEN_LENGTH];  // DROP INDEX's.
    int param_count;  // How many `?`s, each a LITERAL_PARAM.

    ExprNode nodes[MAX_NODES];  // Every expression in the statement.
    int node_count;

    union {
        CreateStatement create;
        CreateIndexStatement create_index;
        InsertStatement insert;
        SelectStatement select;
        DeleteStatement delete;
        UpdateStatement update;
    } as;
} Statement;

const char *data_type_name(DataType type);

const char *operator_name(Operator op);

const char *aggregate_name(AggregateFunc func);

const char *constraint_kind_name(ConstraintKind kind);

// The kind must match, or the program stops.
long long literal_as_int(const Literal *lit);
const char *literal_as_text(const Literal *lit);

// Rebuilds the expression as SQL, with brackets only where needed. Cut short
// to fit.
void expr_to_text(const ExprNode *nodes, int node, char *out, size_t size);

// On failure, out must not be used.
int parse(const TokenList *tokens, Statement *out, Error *error);

#endif
