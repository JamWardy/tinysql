#ifndef TINYSQL_TABLE_H
#define TINYSQL_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "btree.h"
#include "index.h"
#include "parser.h"  // For DataType, ColumnDef and ConstraintKind.
#include "value.h"

// Ownership: a Database owns its Tables, and a Table owns every string and
// array reachable from it. Functions that take strings or Values copy them.
//
// Arrays move when they grow or shrink, so a Table * is valid only until the
// next database_create_table or database_drop_table, and a Row * only until the
// next table_append_row or table_delete_row on its table. Indexes hold row
// positions, not pointers.

typedef struct {
    char *name;
    DataType type;
    int not_null;  // 1 for NOT NULL; the executor keeps NULLs out.
} Column;

typedef struct {
    Value *values;
    size_t count;  // Always the table's column_count.
} Row;

// Columns by their position in a row.
typedef struct {
    size_t positions[MAX_COLUMNS];
    size_t count;
} ColumnList;

// A PRIMARY KEY or UNIQUE key. A UNIQUE key with a NULL in any column isn't
// checked, as in standard SQL, and its row stays out of the index.
typedef struct {
    ConstraintKind kind;
    ColumnList columns;  // In key order.
    Index rows_by_key;   // Each row's position, under its key's hash.
} Constraint;

// Per table, from CREATE INDEX.
#define MAX_INDEXES 16

// From CREATE INDEX: an entry per row, keyed by copies of its values in
// columns.
typedef struct {
    char *name;
    ColumnList columns;
    BTree tree;
} TreeIndex;

typedef struct {
    char *name;
    Column *columns;
    size_t column_count;

    Constraint *constraints;  // NULL when there are none.
    size_t constraint_count;

    TreeIndex indexes[MAX_INDEXES];  // In the order they were created.
    size_t index_count;

    Row *rows;
    size_t row_count;
    size_t row_capacity;

    // 1 if INSERT chooses IDs for the PRIMARY KEY, which is then one INT
    // column.
    int autoincrement;

    // The highest ID the key has had, given or chosen, so a deleted row's ID is
    // never chosen again. 0 without autoincrement.
    long long last_id;
} Table;

typedef struct {
    Table *tables;
    size_t count;
    size_t capacity;

    int in_transaction;  // 1 between BEGIN and COMMIT or ROLLBACK.
    int unsaved;         // 1 if a change failed to save to the file.
} Database;

void database_init(Database *db);

void database_free(Database *db);

Table *database_find_table(const Database *db, const char *name);

// Doesn't check for duplicate table or column names; the executor does.
Table *database_create_table(Database *db, const char *name,
                             const ColumnDef *columns, size_t column_count);

void database_drop_table(Database *db, Table *table);

// Sets *table and *index to the positions of the index's table in db and of the
// index in its table.
int database_find_index(const Database *db, const char *name,
                        size_t *table, size_t *index);

int table_find_column(const Table *table, const char *name, size_t *index);

// columns must be valid, distinct positions. PRIMARY KEY also makes its columns
// NOT NULL. The table must have no rows: nothing checks existing rows against
// it.
int table_add_constraint(Table *table, ConstraintKind kind,
                         const ColumnList *columns);

// The PRIMARY KEY's one column, if the table chooses IDs.
int table_autoincrement_column(const Table *table, size_t *column);

// Builds it from every row. columns must be valid, distinct positions; the
// executor checks the name is free and the table has room.
int table_add_index(Table *table, const char *name,
                    const ColumnList *columns);

void table_drop_index(Table *table, size_t index);

// Uses the key's index. A key with a NULL is never found.
//
// Here and in key_has_null, key_hash and key_equal, values is laid out like a
// row and only the key's columns are read, so a key can be checked straight
// from a row, or from values about to be inserted.
int table_find_key(const Table *table, const Constraint *constraint,
                   const Value *values, size_t *row);

int column_list_has(const ColumnList *list, size_t column);

int key_has_null(const ColumnList *key, const Value *values);

// None of the key's values may be NULL.
uint64_t key_hash(const ColumnList *key, const Value *values);

int key_equal(const ColumnList *key, const Value *a, const Value *b);

// Copies values, which the caller has checked: each NULL or of its column's
// type, with no key already in the table. Updates every index, and last_id.
int table_append_row(Table *table, const Value *values);

// Copies value, which the caller has checked. Doesn't check keys, as they may
// clash partway through an UPDATE. Updates every index, and last_id.
int table_set_value(Table *table, size_t row, size_t column,
                    const Value *value);

// Later rows move down one place, keeping their order.
void table_delete_row(Table *table, size_t index);

#endif
