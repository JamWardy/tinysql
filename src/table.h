#ifndef TINYSQL_TABLE_H
#define TINYSQL_TABLE_H

#include <stddef.h>

#include "parser.h"  // For DataType and ColumnDef.

// Ownership: a Database owns its Tables, and a Table owns every string
// and array reachable from it. Functions that take strings or Values copy
// them, so callers (e.g. the parser's reusable Statement) keep their own.
//
// Arrays grow with realloc, which may move them. A Table * is valid only
// until the next database_create_table, and a Row * only until the next
// table_append_row or table_delete_row on its table; look them up again
// after any of these.

typedef struct {
    char *name;
    DataType type;
} Column;

// A stored value. When type is TYPE_TEXT, as.text is owned by the Row.
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

typedef struct {
    char *name;
    Column *columns;
    size_t column_count;

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

// Returns the index of the column with exactly this name, or -1.
int table_find_column(const Table *table, const char *name);

// Appends a row with copies of values, which must hold column_count
// values whose types match the columns; the caller validates first.
// Returns 1, or 0 if memory ran out (table is unchanged).
int table_append_row(Table *table, const Value *values);

// Frees the row at index and moves every later row down one place, so
// the rest keep their order. index must be less than row_count.
void table_delete_row(Table *table, size_t index);

#endif
