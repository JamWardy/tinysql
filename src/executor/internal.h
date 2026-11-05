#ifndef TINYSQL_EXECUTOR_INTERNAL_H
#define TINYSQL_EXECUTOR_INTERNAL_H

// Shared by the executor's files and included nowhere else; the rest of TinySQL
// includes executor.h.

#include <stddef.h>

#include "executor.h"  // For ParamList.
#include "like.h"
#include "parser.h"
#include "result.h"
#include "table.h"

// Pointers into the table, valid only until it next changes. Whoever fills one
// frees rows.
typedef struct {
    const Row **rows;
    size_t count;
} RowList;

// A statement's expressions checked against its table: each node's type, and
// each column node's position in a row. expr_resolve fills it one tree at a
// time.
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

// A resolved WHERE: a BOOL expression in ctx.
typedef struct {
    const ExprContext *ctx;
    int root;
} Filter;

// In ascending order, valid only until the table next changes. Whoever fills
// one frees positions.
typedef struct {
    size_t *positions;
    size_t count;
} PositionList;

// The order a SELECT wants its rows in, and how many: what decides whether a
// plan giving rows in order can skip the sort and stop early.
typedef enum {
    ORDER_BY_NOTHING,     // No ORDER BY, or an aggregate SELECT's.
    ORDER_BY_EXPRESSION,  // Only a sort can give it.
    ORDER_BY_COLUMN       // A plain column, which an index can give too.
} OrderBy;

typedef struct {
    OrderBy by;
    size_t column;     // ORDER_BY_COLUMN's.
    int descending;
    int has_limit;
    long long limit;
} RowOrder;

// Statements, one per file; execute() picks one by the statement's type.

// schema.c: CREATE and DROP, of tables and indexes.
int execute_create(Database *db, const Statement *stmt,
                   char *error, size_t error_size);
int execute_drop(Database *db, const Statement *stmt,
                 char *error, size_t error_size);
int execute_create_index(Database *db, const Statement *stmt,
                         char *error, size_t error_size);
int execute_drop_index(Database *db, const Statement *stmt,
                       char *error, size_t error_size);

// analyze.c: statistics for one table, or every table.
int execute_analyze(Database *db, const Statement *stmt,
                    char *error, size_t error_size);

// explain.c: for EXPLAIN, fills result with a row per plan considered.
int execute_explain(Database *db, const Statement *stmt,
                    const ParamList *params, Result *result,
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

// select.c: a SELECT list, which INSERT's RETURNING shares.

// place names the list in messages, such as "RETURNING".
int select_resolve_list(ExprContext *ctx, const SelectList *list,
                        int allowed, const char *place,
                        char *error, size_t error_size);

// A column per item, or per table column for `*`. An item is headed by its AS
// name, its column's name, or else its expression rebuilt as text.
void select_add_columns(const ExprContext *ctx, const SelectList *list,
                        Result *result);

// row is NULL for an aggregate SELECT, whose items read only the aggregates
// already worked out.
int select_append_row(const ExprContext *ctx, const SelectList *list,
                      const Row *row, Result *result,
                      char *error, size_t error_size);

// Whether an item, or the ORDER BY, has an aggregate, so the SELECT gives one
// row for all it matches.
int is_aggregate_select(const Statement *stmt);

// The order stmt's SELECT wants, its ORDER BY already resolved in ctx; an
// aggregate SELECT's is none. Fails on a bad LIMIT.
int find_row_order(const ExprContext *ctx, const Statement *stmt,
                   const ParamList *params, RowOrder *out,
                   char *error, size_t error_size);

// common.c: lookups and checks most statements need.

Table *resolve_table(const Database *db, const char *name,
                     char *error, size_t error_size);

// NULL can go in any column. what names the value in the message, e.g. "WHERE
// value".
int check_type(const Column *column, DataType given, const char *what,
               char *error, size_t error_size);

// As check_type, but also refuses NULL in a NOT NULL column, for INSERT
// and UPDATE. A WHERE uses check_type, as IS NULL is fine on any column.
int check_value(const Column *column, DataType given,
                char *error, size_t error_size);

int resolve_column(const Table *table, const char *name, size_t *index,
                   char *error, size_t error_size);

// Its own, or for a `?` its parameter's.
DataType literal_type(const Literal *lit, const ParamList *params);

// Its own, or for a `?` its parameter's. Its text points into lit or the
// parameter, so it's only for functions that copy it.
Value literal_to_value(const Literal *lit, const ParamList *params);

// e.g. "duplicate primary key in table 'users': id = 5". values is laid out
// like a row.
int fail_duplicate(const Table *table, const Constraint *constraint,
                   const Value *values, char *error, size_t error_size);

// expr.c: checking and evaluating expressions.

// stmt, table and params must outlive ctx.
void expr_init(ExprContext *ctx, const Statement *stmt, const Table *table,
               const ParamList *params);

// Checks every column exists and is allowed, and each operator gets the types
// it needs, filling in ctx->types[node]. allowed holds EXPR_ flags; place names
// where the expression is, such as "VALUES", for messages.
int expr_resolve(ExprContext *ctx, int node, int allowed, const char *place,
                 char *error, size_t error_size);

// row may be NULL if the expression reads no columns. Fails on overflow or
// division by zero. Text in *out points into row, the statement or a parameter,
// so read it straight away.
int expr_evaluate(const ExprContext *ctx, int node, const Row *row,
                  Value *out, char *error, size_t error_size);

// The pattern of the EXPR_LIKE at node, with its escape, for row. Sets *none
// if either is NULL, so nothing matches. Fails on a bad escape or pattern.
int expr_like_pattern(const ExprContext *ctx, int node, const Row *row,
                      LikePattern *out, int *none,
                      char *error, size_t error_size);

// For expr_evaluate to read afterwards.
int expr_compute_aggregates(ExprContext *ctx, int node, const RowList *rows,
                            char *error, size_t error_size);

int expr_has_aggregate(const ExprNode *nodes, int node);

int expr_uses_columns(const ExprNode *nodes, int node);

// plan.c: choosing how to find a statement's rows.

// The conditions on one column that an index can use: comparisons `c op e`, or
// `e op c`, reached through ANDs alone, where e reads no columns.
typedef struct {
    int equal;            // e's node in `c = e`, or -1.
    int lower;            // e's node in `c > e` or `c >= e`, or -1.
    int lower_inclusive;  // 1 for >=.
    int upper;            // e's node in `c < e` or `c <= e`, or -1.
    int upper_inclusive;  // 1 for <=.
    int like;             // `c LIKE e`'s node, whose prefix bounds c, or -1.
} IndexableConditions;

typedef enum {
    PLAN_SCAN,   // Read every row.
    PLAN_KEY,    // Find one row through a key's hash index.
    PLAN_INDEX   // Read a range of a CREATE INDEX index.
} PlanKind;

// Whether a plan gives its rows in the ORDER BY's order, and how.
typedef enum {
    ORDERING_NONE,       // They need sorting.
    ORDERING_FORWARDS,   // In order as found.
    ORDERING_BACKWARDS   // In order, walking the index backwards, for DESC.
} PlanOrdering;

// One way of finding a statement's rows, with its estimates.
typedef struct {
    PlanKind kind;
    const Constraint *constraint;  // PLAN_KEY's key, whose every column
                                   // the WHERE pins with =.
    const TreeIndex *index;        // PLAN_INDEX's index, how many of its
    size_t equal_count;            // leading columns are pinned, and its
    const IndexableConditions *range;  // next column's conditions, or NULL.
    int like;                      // With range, the LIKE whose prefix gives
                                   // its bounds instead, or -1.
    PlanOrdering ordering;
    double reads;                  // Estimated rows read, LIMIT included.
    double cost;                   // A sort included, if it needs one.
} Plan;

// Reading every row, then a plan per key and per index at most.
#define MAX_PLANS (1 + MAX_KEYS + MAX_INDEXES)

// Plans point into indexable, so a CandidatePlans mustn't be copied.
typedef struct {
    IndexableConditions indexable[MAX_COLUMNS];  // Per column of the table.
    Plan plans[MAX_PLANS];             // plans[0] reads every row.
    size_t count;
    size_t chosen;
    double matches;  // Estimated rows the whole WHERE matches.
    int sorted;      // There's an ORDER BY to give.
} CandidatePlans;

// filter may be NULL, for every row, and order NULL when the order doesn't
// matter. Reads no rows, but with statistics it works out the values
// conditions compare with.
void choose_plan(const Table *table, const Filter *filter,
                 const RowOrder *order, CandidatePlans *candidates);

// where.c: checking a WHERE and finding the rows it matches.

// Sets *out to filter, or to NULL (every row) when has_where is 0. Checks
// columns and types before any row is read, so a mistake is found even where no
// row would reach it. ctx must outlive filter.
int where_resolve(ExprContext *ctx, int has_where, int root, Filter *filter,
                  const Filter **out, char *error, size_t error_size);

// In table order, by the plan choose_plan picks.
int where_collect_positions(const Table *table, const Filter *filter,
                            PositionList *out, char *error, size_t error_size);

// out->rows is a new array, which the caller frees. With an order, sets
// *in_order to whether the rows came in it, and stops at its LIMIT if so.
int where_collect_rows(const Table *table, const Filter *filter,
                       const RowOrder *order, RowList *out, int *in_order,
                       char *error, size_t error_size);

#endif
