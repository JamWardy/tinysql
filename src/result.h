#ifndef TINYSQL_RESULT_H
#define TINYSQL_RESULT_H

#include <stddef.h>

#include "parser.h"  // For DataType, StatementType and MAX_COLUMNS.
#include "table.h"   // For Value.

// Room for a heading such as "COUNT(name)" or "price * quantity"; longer ones
// are cut short.
#define MAX_HEADING (MAX_TOKEN_LENGTH + 8)

// A SELECT, or an INSERT with RETURNING, fills the columns and rows; INSERT,
// UPDATE and DELETE set changed. The Result owns copies of its values, so they
// stay valid until result_free, however the database changes.
typedef struct {
    StatementType type;  // The statement that filled it; unset while empty.

    char names[MAX_COLUMNS][MAX_HEADING];
    DataType types[MAX_COLUMNS];  // Values may be NULL too.
    size_t column_count;

    Value *values;  // row_count rows of column_count values, row by row.
    size_t row_count;
    size_t row_capacity;

    size_t changed;  // Rows inserted, updated or deleted.
} Result;

void result_init(Result *result);

void result_free(Result *result);

// name is cut short to fit. Every column comes before any row.
void result_add_column(Result *result, const char *name, DataType type);

// Copies values, which holds column_count values.
int result_append_row(Result *result, const Value *values);

const Value *result_value(const Result *result, size_t row, size_t column);

#endif
