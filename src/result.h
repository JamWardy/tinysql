#ifndef TINYSQL_RESULT_H
#define TINYSQL_RESULT_H

#include <stddef.h>

#include "parser.h"  // For DataType, StatementType and MAX_COLUMNS.
#include "table.h"   // For Value.

// Room for a column name, or an aggregate heading such as "COUNT(name)":
// the longest function name, the column name, "(" and ")".
#define MAX_HEADING (MAX_TOKEN_LENGTH + 8)

// What a statement gives back. A SELECT fills the columns and rows; the
// other statements leave them empty and set changed instead.
//
// The Result owns copies of its values, so they stay valid however the
// database changes, until result_free.
typedef struct {
    StatementType type;  // The statement that filled it; unset while empty.

    char names[MAX_COLUMNS][MAX_HEADING];
    DataType types[MAX_COLUMNS];  // Each column's type; values may be NULL.
    size_t column_count;

    Value *values;  // row_count rows of column_count values, row by row.
    size_t row_count;
    size_t row_capacity;

    size_t changed;  // Rows inserted, updated or deleted.
} Result;

// Leaves result empty: no columns, no rows, nothing changed.
void result_init(Result *result);

// Frees result's values and leaves it empty, ready to reuse.
void result_free(Result *result);

// Adds a column called name, cut short to fit, holding type. The result
// must have fewer than MAX_COLUMNS columns and no rows yet.
void result_add_column(Result *result, const char *name, DataType type);

// Appends a row with copies of values, which must hold column_count
// values. Returns 1, or 0 if memory ran out (result is unchanged).
int result_append_row(Result *result, const Value *values);

// Returns the value in row at column, which must both be in range.
const Value *result_value(const Result *result, size_t row, size_t column);

#endif
