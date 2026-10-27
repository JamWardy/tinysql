#ifndef TINYSQL_TABLE_H
#define TINYSQL_TABLE_H

#include <stddef.h>

#include "parser.h"  // For DataType and ColumnDef.

// Ownership: a Database owns its Tables, and a Table owns every string
// and array reachable from it. Functions that take strings or Values copy
// them, so callers (e.g. the parser's reusable Statement) keep their own.
//
// Arrays grow with realloc, which may move them, and removing an element
// moves the ones after it. A Table * is valid only until the next
// database_create_table or database_drop_table, and a Row * only until
// the next table_append_row or table_delete_row on its table; look them
// up again after any of these.

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

// Frees table, which must be one of db's, and moves every later table
// down one place.
void database_drop_table(Database *db, Table *table);

// Looks for the column with exactly this name. If there is one, sets
// *index to its position and returns 1; otherwise returns 0.
int table_find_column(const Table *table, const char *name, size_t *index);

// Frees the text owned by the first count values, which may be NULL or
// any type. The array itself is the caller's.
void values_free(Value *values, size_t count);

// Appends a row with copies of values, which must hold column_count
// values, each NULL or of its column's type; the caller validates first.
// Returns 1, or 0 if memory ran out (table is unchanged).
int table_append_row(Table *table, const Value *values);

// Replaces one value of a row with a copy of value, which must be NULL
// or of the column's type; the caller validates first.
// Returns 1, or 0 if memory ran out (the old value is kept).
int table_set_value(Table *table, size_t row, size_t column,
                    const Value *value);

// Frees the row at index and moves every later row down one place, so
// the rest keep their order. index must be less than row_count.
void table_delete_row(Table *table, size_t index);

#endif
