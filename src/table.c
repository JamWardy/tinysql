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

void values_free(Value *values, size_t count) {
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
        values_free(table->rows[i].values, table->rows[i].count);
        free(table->rows[i].values);
    }

    for (size_t i = 0; i < table->constraint_count; i++) {
        index_free(&table->constraints[i].rows_by_key);
    }
    free(table->constraints);

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
        table->columns[i].not_null = columns[i].not_null;
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

int table_find_column(const Table *table, const char *name, size_t *index) {
    for (size_t i = 0; i < table->column_count; i++) {
        if (strcmp(table->columns[i].name, name) == 0) {
            *index = i;
            return 1;
        }
    }

    return 0;
}

int table_add_constraint(Table *table, ConstraintKind kind,
                         const ColumnList *columns) {
    assert(table->row_count == 0);
    assert(columns->count > 0 && columns->count <= table->column_count);

    Constraint *constraints = realloc(
        table->constraints,
        (table->constraint_count + 1) * sizeof(*constraints));
    if (!constraints) {
        return 0;
    }
    table->constraints = constraints;

    Constraint *constraint = &constraints[table->constraint_count++];
    constraint->kind = kind;
    constraint->columns = *columns;
    index_init(&constraint->rows_by_key);

    switch (kind) {
        case CONSTRAINT_PRIMARY_KEY:
            // A key identifies its row, and NULL is no value, so it
            // can't identify anything.
            for (size_t i = 0; i < columns->count; i++) {
                assert(columns->index[i] < table->column_count);
                table->columns[columns->index[i]].not_null = 1;
            }
            break;
    }
    return 1;
}

int column_list_has(const ColumnList *list, size_t column) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->index[i] == column) {
            return 1;
        }
    }
    return 0;
}

// FNV-1a, a simple hash that's good enough for an in-memory index: for
// each byte, mix it in with XOR, then multiply to spread it through all
// 64 bits. The constants are the published 64-bit ones.
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

static uint64_t hash_byte(uint64_t hash, unsigned char byte) {
    return (hash ^ byte) * FNV_PRIME;
}

uint64_t key_hash(const ColumnList *key, const Value *values) {
    uint64_t hash = FNV_OFFSET;

    for (size_t i = 0; i < key->count; i++) {
        const Value *value = &values[key->index[i]];

        // A bug in the caller: the key's columns are NOT NULL.
        assert(value->type != TYPE_NULL);

        if (value->type == TYPE_INT) {
            // Byte by byte from the value, not its memory, so the hash
            // doesn't depend on the machine's byte order.
            uint64_t n = (uint64_t)value->as.integer;
            for (int b = 0; b < 8; b++) {
                hash = hash_byte(hash, (unsigned char)(n >> (8 * b)));
            }
        } else {
            for (const char *c = value->as.text; *c != '\0'; c++) {
                hash = hash_byte(hash, (unsigned char)*c);
            }
            // End each text with a 0, so the key ('ab', 'c') doesn't
            // hash the same as ('a', 'bc').
            hash = hash_byte(hash, 0);
        }
    }

    // Multiplying only carries upwards, so the low bits, which the
    // index uses to pick a slot, have seen the least mixing. Folding
    // the high half into the low half fixes that.
    return hash ^ (hash >> 32);
}

int key_equal(const ColumnList *key, const Value *a, const Value *b) {
    for (size_t i = 0; i < key->count; i++) {
        const Value *x = &a[key->index[i]];
        const Value *y = &b[key->index[i]];

        assert(x->type == y->type && x->type != TYPE_NULL);

        if (x->type == TYPE_INT ? x->as.integer != y->as.integer
                                : strcmp(x->as.text, y->as.text) != 0) {
            return 0;
        }
    }
    return 1;
}

// What table_find_key is looking for, passed through index_find.
typedef struct {
    const Table *table;
    const ColumnList *key;
    const Value *values;
} KeySearch;

static int row_has_key(size_t row, const void *context) {
    const KeySearch *search = context;

    return key_equal(search->key, search->table->rows[row].values,
                     search->values);
}

int table_find_key(const Table *table, const Constraint *constraint,
                   const Value *values, size_t *row) {
    KeySearch search = { table, &constraint->columns, values };

    return index_find(&constraint->rows_by_key,
                      key_hash(&constraint->columns, values),
                      row_has_key, &search, row);
}

int table_append_row(Table *table, const Value *values) {
    // Make room in every index first. A failure partway leaves some
    // indexes bigger than needed, which is harmless.
    for (size_t i = 0; i < table->constraint_count; i++) {
        if (!index_reserve(&table->constraints[i].rows_by_key,
                           table->row_count + 1)) {
            return 0;
        }
    }

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
        assert(values[i].type == table->columns[i].type ||
               values[i].type == TYPE_NULL);

        copy[i].type = values[i].type;

        if (values[i].type == TYPE_TEXT) {
            copy[i].as.text = copy_string(values[i].as.text);

            if (!copy[i].as.text) {
                values_free(copy, i);
                free(copy);
                return 0;
            }
        } else if (values[i].type == TYPE_INT) {
            copy[i].as.integer = values[i].as.integer;
        }
    }

    for (size_t i = 0; i < table->constraint_count; i++) {
        Constraint *constraint = &table->constraints[i];

        index_insert(&constraint->rows_by_key,
                     key_hash(&constraint->columns, copy), table->row_count);
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
    assert(value->type == table->columns[column].type ||
           value->type == TYPE_NULL);

    Value copy = *value;

    if (value->type == TYPE_TEXT) {
        copy.as.text = copy_string(value->as.text);
        if (!copy.as.text) {
            return 0;
        }
    }

    const Value *values = table->rows[row].values;

    // The row is filed under its old key, which is about to change.
    for (size_t i = 0; i < table->constraint_count; i++) {
        Constraint *constraint = &table->constraints[i];

        if (column_list_has(&constraint->columns, column)) {
            index_remove(&constraint->rows_by_key,
                         key_hash(&constraint->columns, values), row);
        }
    }

    // Free the old value only once the new one is ready, so a failure
    // leaves the old one in place. The old value may be NULL, which
    // values_free knows has no text to free.
    Value *slot = &table->rows[row].values[column];
    values_free(slot, 1);
    *slot = copy;

    // Removing the row made room, so these can't fail.
    for (size_t i = 0; i < table->constraint_count; i++) {
        Constraint *constraint = &table->constraints[i];

        if (column_list_has(&constraint->columns, column)) {
            index_insert(&constraint->rows_by_key,
                         key_hash(&constraint->columns, values), row);
        }
    }
    return 1;
}

void table_delete_row(Table *table, size_t index) {
    assert(index < table->row_count);

    Row *row = &table->rows[index];

    // Every later row moves down one place, so its position in each
    // index does too. This reads every slot, so deleting a row is O(n),
    // as the memmove below already is.
    for (size_t i = 0; i < table->constraint_count; i++) {
        Constraint *constraint = &table->constraints[i];

        index_remove(&constraint->rows_by_key,
                     key_hash(&constraint->columns, row->values), index);
        index_shift_rows(&constraint->rows_by_key, index);
    }

    values_free(row->values, row->count);
    free(row->values);

    // Close the gap. The array keeps its capacity for later inserts.
    memmove(row, row + 1,
            (table->row_count - index - 1) * sizeof(*row));
    table->row_count--;
}
