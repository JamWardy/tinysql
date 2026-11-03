#ifndef TINYSQL_EXECUTOR_INTERNAL_H
#define TINYSQL_EXECUTOR_INTERNAL_H

// Shared between the executor's files in this folder, and not for use
// outside it; the rest of TinySQL includes executor.h. Each statement
// has its own file, and calls into common.c and where.c.

#include <stddef.h>

#include "executor.h"  // For ParamList.
#include "parser.h"
#include "result.h"
#include "table.h"

// A WHERE condition tree checked and resolved against a table.
typedef struct {
    const Condition *conditions;     // The statement's nodes.
    size_t columns[MAX_CONDITIONS];  // Per COND_COMPARE node, its column.
    int root;
    const ParamList *params;         // The values of any `?`s.
} Filter;

// The rows a query matched. These are pointers into the table, not
// copies, so they're valid only until the table next changes. Whoever
// fills one frees rows when done.
typedef struct {
    const Row **rows;
    size_t count;
} RowList;

// A statement's expressions checked against its table, ready to
// evaluate: each node's type, and each column node's position in a row.
// Filled by expr_resolve, one tree at a time.
typedef struct {
    const ExprNode *nodes;         // The statement's.
    const Table *table;
    const ParamList *params;       // The values of any `?`s.
    DataType types[MAX_NODES];     // Per resolved node; NULL for NULL.
    size_t columns[MAX_NODES];     // Per EXPR_COLUMN node.
    Value aggregates[MAX_NODES];   // Per EXPR_AGGREGATE node, once computed.
} ExprContext;

// What an expression may use, for expr_resolve.
#define EXPR_COLUMNS 1     // Columns of the row being read.
#define EXPR_AGGREGATES 2  // Aggregates, and columns only inside them.

// Positions of rows in a table, in ascending order. Valid only until
// the table next changes. Whoever fills one frees positions when done.
typedef struct {
    size_t *positions;
    size_t count;
} PositionList;

// Statements, one per file: execute() in executor.c picks the one for
// the statement's type. Each takes the statement and the database, and
// on failure writes a message to error and returns 0.

// schema.c
int execute_create(Database *db, const Statement *stmt,
                   char *error, size_t error_size);
int execute_drop(Database *db, const Statement *stmt,
                 char *error, size_t error_size);

// insert.c, select.c, delete.c and update.c fill result, and read any
// `?`s' values from params.
int execute_insert(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size);
int execute_select(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size);
int execute_delete(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size);
int execute_update(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size);

// common.c: lookups and checks most statements need.

// Returns the table called name, or NULL after writing an error.
Table *common_find_table(const Database *db, const char *name,
                         char *error, size_t error_size);

// Checks that a value of type given can go in column. NULL can go in
// any column. what names the value in the error, e.g. "value" or
// "WHERE value".
int common_check_type(const Column *column, DataType given, const char *what,
                      char *error, size_t error_size);

// Checks that a value of type given can be stored in column by INSERT
// or UPDATE: it has the right type, and isn't NULL if the column is NOT
// NULL. A WHERE uses common_check_type instead, since IS NULL is fine
// on any column.
int common_check_value(const Column *column, DataType given,
                       char *error, size_t error_size);

// Returns <0, 0 or >0 as a is less than, equal to or greater than b,
// which must have the same type and not be NULL. Text compares byte by
// byte, so 'B' < 'a' and '10' < '9'.
int common_compare_values(const Value *a, const Value *b);

// Sets *index to the position of the column called name in table, the
// position of its value in each row. Every name the SQL uses is looked
// up this way once, before any row is read.
int common_find_column(const Table *table, const char *name, size_t *index,
                       char *error, size_t error_size);

// The type of lit's value: its own, or for a `?` its parameter's.
DataType common_literal_type(const Literal *lit, const ParamList *params);

// The Value to pass to the table for lit: its own, or for a `?` its
// parameter's. Its text points into lit or the parameter, so it's only
// for functions that copy it, and both must outlive it.
Value common_literal_to_value(const Literal *lit, const ParamList *params);

// Fails with the message for a second row with values' key, e.g.
// "duplicate primary key in table 'users': id = 5". values is laid out
// like a row.
int common_fail_duplicate(const Table *table, const Constraint *constraint,
                          const Value *values, char *error, size_t error_size);

// expr.c: checking and evaluating expressions.

// Readies ctx for stmt's expressions over table, with params for `?`s.
// stmt, table and params must outlive ctx.
void expr_init(ExprContext *ctx, const Statement *stmt, const Table *table,
               const ParamList *params);

// Checks the expression at node and fills in its types and columns:
// every column exists and is allowed, and each operator gets the types
// it needs. allowed holds EXPR_ flags, and place names where the
// expression is, such as "VALUES", for messages. Then ctx->types[node]
// is its type.
int expr_resolve(ExprContext *ctx, int node, int allowed, const char *place,
                 char *error, size_t error_size);

// Sets *out to the value of the resolved expression at node for row,
// which may be NULL if the expression reads no columns. Fails on
// overflow or division by zero. Text in *out points into row, the
// statement or a parameter, so it's only for reading straight away.
int expr_evaluate(const ExprContext *ctx, int node, const Row *row,
                  Value *out, char *error, size_t error_size);

// Works out every aggregate in the resolved expression at node over
// rows, for expr_evaluate to read.
int expr_compute_aggregates(ExprContext *ctx, int node, const RowList *rows,
                            char *error, size_t error_size);

// Returns 1 if the expression at node has an aggregate in it.
int expr_has_aggregate(const ExprNode *nodes, int node);

// Returns 1 if the expression at node reads any column, so its value
// may differ from row to row.
int expr_uses_columns(const ExprNode *nodes, int node);

// where.c: checking a WHERE and finding the rows it matches.

// Checks every comparison in where against table and fills filter, then
// sets *out to filter, or to NULL (matching every row) when has_where
// is 0. Done before any row is read, so a bad column or type is
// reported even when no row would reach that part of the condition.
// params holds the values of any `?`s, and must outlive filter.
int where_resolve(const Table *table, const ParamList *params, int has_where,
                  const WhereClause *where, Filter *filter,
                  const Filter **out, char *error, size_t error_size);

// Fills out with the positions of the rows filter matches, in table
// order, through a key's index when the filter allows it.
int where_collect_positions(const Table *table, const Filter *filter,
                            PositionList *out, char *error, size_t error_size);

// Fills out with the rows filter matches, in table order. out->rows is
// a new array, which the caller frees.
int where_collect_rows(const Table *table, const Filter *filter, RowList *out,
                       char *error, size_t error_size);

#endif
