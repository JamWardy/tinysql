#include "result.h"

#include <assert.h>
#include <stdlib.h>

#include "text.h"

void result_init(Result *result) {
    result->column_count = 0;
    result->values = NULL;
    result->row_count = 0;
    result->row_capacity = 0;
    result->changed = 0;
}

void result_free(Result *result) {
    values_free(result->values, result->row_count * result->column_count);
    free(result->values);
    result_init(result);
}

void result_add_column(Result *result, const char *name, DataType type) {
    assert(result->column_count < MAX_COLUMNS && result->row_count == 0);

    size_t i = result->column_count++;
    copy_text(result->names[i], sizeof(result->names[i]), name);
    result->types[i] = type;
}

int result_append_row(Result *result, const Value *values) {
    size_t width = result->column_count;

    if (result->row_count == result->row_capacity) {
        size_t capacity = result->row_capacity == 0
                              ? 8 : result->row_capacity * 2;
        // A row with no columns needs no memory, but malloc(0) may give
        // NULL, so always ask for at least one value.
        size_t size = (width > 0 ? width : 1) * capacity * sizeof(Value);
        Value *grown = realloc(result->values, size);

        if (!grown) {
            return 0;
        }
        result->values = grown;
        result->row_capacity = capacity;
    }

    Value *row = &result->values[result->row_count * width];

    for (size_t i = 0; i < width; i++) {
        if (!value_copy(&row[i], &values[i])) {
            values_free(row, i);
            return 0;
        }
    }

    result->row_count++;
    return 1;
}

const Value *result_value(const Result *result, size_t row, size_t column) {
    assert(row < result->row_count && column < result->column_count);
    return &result->values[row * result->column_count + column];
}
