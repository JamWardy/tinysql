#include "internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "error.h"

Table *common_find_table(const Database *db, const char *name,
                         char *error, size_t error_size) {
    Table *table = database_find_table(db, name);

    if (!table) {
        fail(error, error_size, "no such table '%s'", name);
    }
    return table;
}

int common_check_type(const Column *column, DataType given, const char *what,
                      char *error, size_t error_size) {
    if (given != TYPE_NULL && given != column->type) {
        return fail(error, error_size, "column '%s' is %s but the %s is %s",
             column->name, data_type_name(column->type), what,
             data_type_name(given));
    }
    return 1;
}

int common_check_value(const Column *column, DataType given,
                       char *error, size_t error_size) {
    if (given == TYPE_NULL && column->not_null) {
        return fail(error, error_size, "column '%s' can't be NULL",
             column->name);
    }
    return common_check_type(column, given, "value", error, error_size);
}

int common_compare_values(const Value *a, const Value *b) {
    switch (a->type) {
        case TYPE_INT: {
            long long x = a->as.integer;
            long long y = b->as.integer;
            return (x > y) - (x < y);
        }
        case TYPE_TEXT:
            return strcmp(a->as.text, b->as.text);
        case TYPE_BOOL:
            return a->as.boolean - b->as.boolean;  // false < true.
        case TYPE_NULL:
            break;  // Callers handle NULL first.
    }
    return 0;
}

int common_find_column(const Table *table, const char *name, size_t *index,
                       char *error, size_t error_size) {
    if (!table_find_column(table, name, index)) {
        return fail(error, error_size, "no such column '%s'", name);
    }
    return 1;
}

DataType common_literal_type(const Literal *lit, const ParamList *params) {
    return common_literal_to_value(lit, params).type;
}

Value common_literal_to_value(const Literal *lit, const ParamList *params) {
    Value value;

    switch (lit->kind) {
        case LITERAL_INT:
            value.type = TYPE_INT;
            value.as.integer = literal_as_int(lit);
            break;
        case LITERAL_TEXT:
            // Value.as.text isn't const because the table frees its own
            // copies; the table functions only read this one and copy it.
            value.type = TYPE_TEXT;
            value.as.text = (char *)literal_as_text(lit);
            break;
        case LITERAL_BOOL:
            value.type = TYPE_BOOL;
            value.as.boolean = lit->as.boolean;
            break;
        case LITERAL_NULL:
            value.type = TYPE_NULL;
            break;
        case LITERAL_PARAM:
            // execute has checked there's a value for every `?`.
            assert((size_t)lit->as.param < params->count);
            value = params->values[lit->as.param];
            break;
    }
    return value;
}

static const char *constraint_name(ConstraintKind kind) {
    switch (kind) {
        case CONSTRAINT_PRIMARY_KEY: return "primary key";
        case CONSTRAINT_UNIQUE:      return "unique key";
    }
    return "key";
}

int common_fail_duplicate(const Table *table, const Constraint *constraint,
                          const Value *values, char *error, size_t error_size) {
    const ColumnList *key = &constraint->columns;
    char text[256];  // Cut short if it's long; it's only for the message.
    size_t length = 0;

    text[0] = '\0';
    for (size_t i = 0; i < key->count && length < sizeof(text); i++) {
        const Column *column = &table->columns[key->positions[i]];
        const Value *value = &values[key->positions[i]];
        const char *separator = i > 0 ? ", " : "";
        int written;

        if (value->type == TYPE_INT) {
            written = snprintf(text + length, sizeof(text) - length,
                               "%s%s = %lld", separator, column->name,
                               value->as.integer);
        } else {
            written = snprintf(text + length, sizeof(text) - length,
                               "%s%s = '%s'", separator, column->name,
                               value->as.text);
        }
        // snprintf returns the length it wanted, which may not have fit;
        // then length passes the end and the loop stops.
        length += written > 0 ? (size_t)written : 0;
    }

    return fail(error, error_size, "duplicate %s in table '%s': %s",
                constraint_name(constraint->kind), table->name, text);
}
