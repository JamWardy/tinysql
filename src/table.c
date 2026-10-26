#include "table.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// strdup isn't part of C11, so copy strings by hand.
static char *copy_string(const char *s) {
    size_t size = strlen(s) + 1;
    char *copy = malloc(size);

    if (copy) {
        memcpy(copy, s, size);
    }
    return copy;
}

// Frees the strings in the first count values of a row.
static void free_values(Value *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (values[i].type == TYPE_TEXT) {
            free(values[i].as.text);
        }
    }
}

static void free_columns(Column *columns, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(columns[i].name);
    }
    free(columns);
}

static void table_free(Table *table) {
    for (size_t i = 0; i < table->row_count; i++) {
        free_values(table->rows[i].values, table->rows[i].count);
        free(table->rows[i].values);
    }

    free(table->rows);
    free_columns(table->columns, table->column_count);
    free(table->name);
}

void database_init(Database *db) {
    db->tables = NULL;
    db->count = 0;
    db->capacity = 0;
}

void database_free(Database *db) {
    for (size_t i = 0; i < db->count; i++) {
        table_free(&db->tables[i]);
    }

    free(db->tables);
    database_init(db);
}

Table *database_find_table(const Database *db, const char *name) {
    for (size_t i = 0; i < db->count; i++) {
        if (strcmp(db->tables[i].name, name) == 0) {
            return &db->tables[i];
        }
    }

    return NULL;
}

Table *database_create_table(Database *db, const char *name,
                             const ColumnDef *columns, size_t column_count) {
    // The grammar requires a column, and calloc(0) may return NULL.
    assert(column_count > 0);

    // Make room in db first, so a later failure needs no undoing there.
    if (db->count == db->capacity) {
        size_t capacity = db->capacity == 0 ? 4 : db->capacity * 2;
        Table *tables = realloc(db->tables, capacity * sizeof(*tables));

        if (!tables) {
            return NULL;
        }
        db->tables = tables;
        db->capacity = capacity;
    }

    // Built in the free slot past the end; count only goes up on success,
    // so a failure leaves the slot unused.
    Table *table = &db->tables[db->count];
    memset(table, 0, sizeof(*table));

    table->name = copy_string(name);
    table->columns = calloc(column_count, sizeof(*table->columns));
    if (!table->name || !table->columns) {
        free(table->name);
        free(table->columns);
        return NULL;
    }

    for (size_t i = 0; i < column_count; i++) {
        table->columns[i].type = columns[i].type;
        table->columns[i].name = copy_string(columns[i].name);

        if (!table->columns[i].name) {
            free_columns(table->columns, i);
            free(table->name);
            return NULL;
        }
    }

    table->column_count = column_count;
    db->count++;
    return table;
}

void database_drop_table(Database *db, Table *table) {
    // Pointer subtraction gives the table's index in the array.
    assert(table >= db->tables && table < db->tables + db->count);
    size_t index = (size_t)(table - db->tables);

    table_free(table);
    memmove(table, table + 1,
            (db->count - index - 1) * sizeof(*table));
    db->count--;
}

int table_find_column(const Table *table, const char *name) {
    for (size_t i = 0; i < table->column_count; i++) {
        if (strcmp(table->columns[i].name, name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

int table_append_row(Table *table, const Value *values) {
    if (table->row_count == table->row_capacity) {
        size_t capacity = table->row_capacity == 0
                              ? 8 : table->row_capacity * 2;
        Row *rows = realloc(table->rows, capacity * sizeof(*rows));

        if (!rows) {
            return 0;
        }
        table->rows = rows;
        table->row_capacity = capacity;
    }

    Value *copy = malloc(table->column_count * sizeof(*copy));
    if (!copy) {
        return 0;
    }

    for (size_t i = 0; i < table->column_count; i++) {
        // A mismatch means the executor skipped validation.
        assert(values[i].type == table->columns[i].type);

        copy[i].type = values[i].type;

        if (values[i].type == TYPE_TEXT) {
            copy[i].as.text = copy_string(values[i].as.text);

            if (!copy[i].as.text) {
                free_values(copy, i);
                free(copy);
                return 0;
            }
        } else {
            copy[i].as.integer = values[i].as.integer;
        }
    }

    Row *row = &table->rows[table->row_count++];
    row->values = copy;
    row->count = table->column_count;
    return 1;
}

int table_set_value(Table *table, size_t row, size_t column,
                    const Value *value) {
    assert(row < table->row_count && column < table->column_count);
    // A mismatch means the executor skipped validation.
    assert(value->type == table->columns[column].type);

    Value *slot = &table->rows[row].values[column];

    if (value->type == TYPE_TEXT) {
        // Copy before freeing, so a failure leaves the old text in place.
        char *text = copy_string(value->as.text);
        if (!text) {
            return 0;
        }
        free(slot->as.text);
        slot->as.text = text;
    } else {
        slot->as.integer = value->as.integer;
    }

    return 1;
}

void table_delete_row(Table *table, size_t index) {
    assert(index < table->row_count);

    Row *row = &table->rows[index];
    free_values(row->values, row->count);
    free(row->values);

    // Close the gap. The array keeps its capacity for later inserts.
    memmove(row, row + 1,
            (table->row_count - index - 1) * sizeof(*row));
    table->row_count--;
}
