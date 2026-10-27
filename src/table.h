#ifndef TINYSQL_TABLE_H
#define TINYSQL_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "index.h"
#include "parser.h"  // For DataType and ColumnDef.

// Ownership: a Database owns its Tables, and a Table owns every string
// and array reachable from it. Functions that take strings or Values copy
// them, so callers (e.g. the parser's reusable Statement) keep their own.
//
// Arrays grow with realloc, which may move them, and removing an element
// moves the ones after it. A Table * is valid only until the next
// database_create_table or database_drop_table, and a Row * only until
// the next table_append_row or table_delete_row on its table; look them
// up again after any of these. Indexes hold row positions, not
// pointers, and table.c keeps them up to date through every change.

typedef struct {
    char *name;
    DataType type;
    int not_null;  // 1 for NOT NULL; the executor keeps NULLs out.
} Column;

// A stored value: its column's type, or TYPE_NULL, for which as is
// unused. When type is TYPE_TEXT, as.text is owned by the Row.
typedef struct {
    DataType type;
    union {
        long long integer;
        char *text;
    } as;
} Value;

typedef struct {
    Value *values;
    size_t count;  // Always the table's column_count.
} Row;

// Columns of a table, by position in each row: a key's columns, or the
// columns a query reads.
typedef struct {
    size_t index[MAX_COLUMNS];
    size_t count;
} ColumnList;

typedef enum {
    CONSTRAINT_PRIMARY_KEY
} ConstraintKind;

// A rule over one or more columns, e.g. PRIMARY KEY (order_id, product_id).
// No two rows have the same key: the same values in all its columns.
typedef struct {
    ConstraintKind kind;
    ColumnList columns;  // The key's columns, in key order.
    Index rows_by_key;   // Every row's position, under its key's hash.
} Constraint;

typedef struct {
    char *name;
    Column *columns;
    size_t column_count;

    Constraint *constraints;  // NULL when there are none.
    size_t constraint_count;

    Row *rows;
    size_t row_count;
    size_t row_capacity;
} Table;

typedef struct {
    Table *tables;
    size_t count;
    size_t capacity;
} Database;

void database_init(Database *db);

// Frees every table and leaves db empty, ready to reuse.
void database_free(Database *db);

// Returns the table with exactly this name, or NULL.
Table *database_find_table(const Database *db, const char *name);

// Adds a table with copies of name and the column definitions, which
// become the table's own Columns. Doesn't check for duplicate table or
// column names; that's the executor's job.
// Returns the new table, or NULL if memory ran out (db is unchanged).
Table *database_create_table(Database *db, const char *name,
                             const ColumnDef *columns, size_t column_count);

// Frees table, which must be one of db's, and moves every later table
// down one place.
void database_drop_table(Database *db, Table *table);

// Looks for the column with exactly this name. If there is one, sets
// *index to its position and returns 1; otherwise returns 0.
int table_find_column(const Table *table, const char *name, size_t *index);

// Adds a constraint over columns, which must be valid, distinct
// positions. PRIMARY KEY also makes its columns NOT NULL. The table
// must have no rows yet; nothing checks existing rows against it.
// Returns 1, or 0 if memory ran out (table is unchanged).
int table_add_constraint(Table *table, ConstraintKind kind,
                         const ColumnList *columns);

// Looks for a row with the same key as values. If there is one, sets
// *row to its position and returns 1; otherwise returns 0. Uses the
// constraint's index, so it doesn't read every row.
//
// Here and in key_hash and key_equal, values is laid out like a row,
// one per column by position, and only the key's columns are read. So a
// key can be checked straight from a row, or from values to be inserted.
int table_find_key(const Table *table, const Constraint *constraint,
                   const Value *values, size_t *row);

// Returns 1 if column is in list.
int column_list_has(const ColumnList *list, size_t column);

// The hash of the values in key's columns, for an Index. values is
// laid out like a row, and none of the key's values may be NULL.
uint64_t key_hash(const ColumnList *key, const Value *values);

// Returns 1 if a and b, each laid out like a row, have the same values
// in every one of key's columns.
int key_equal(const ColumnList *key, const Value *a, const Value *b);

// Frees the text owned by the first count values, which may be NULL or
// any type. The array itself is the caller's.
void values_free(Value *values, size_t count);

// Appends a row with copies of values, which must hold column_count
// values, each NULL or of its column's type, and no key already in the
// table; the caller validates first. Adds the row to every index.
// Returns 1, or 0 if memory ran out (table is unchanged).
int table_append_row(Table *table, const Value *values);

// Replaces one value of a row with a copy of value, which must be NULL
// or of the column's type; the caller validates first. If column is in
// a key, the row moves to its new key in that index. Keys may clash
// partway through an UPDATE, so that isn't checked here.
// Returns 1, or 0 if memory ran out (the old value is kept).
int table_set_value(Table *table, size_t row, size_t column,
                    const Value *value);

// Frees the row at index and moves every later row down one place, so
// the rest keep their order, and updates the indexes to match. index
// must be less than row_count.
void table_delete_row(Table *table, size_t index);

#endif
