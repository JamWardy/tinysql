#include "internal.h"

#include <stdio.h>
#include <string.h>

#include "error.h"

// Checks each is one of the new table's columns, named once.
static int resolve_key(const CreateStatement *create, const KeyDef *key,
                       ColumnList *out, char *error, size_t error_size) {
    const char *kind = constraint_kind_name(key->kind);

    out->count = (size_t)key->column_count;

    for (size_t i = 0; i < out->count; i++) {
        const char *name = key->columns[i];
        int found = 0;

        for (int c = 0; c < create->column_count && !found; c++) {
            if (strcmp(create->columns[c].name, name) == 0) {
                out->positions[i] = (size_t)c;
                found = 1;
            }
        }
        if (!found) {
            return fail(error, error_size,
                        "no such column '%s' in %s", name, kind);
        }

        for (size_t j = 0; j < i; j++) {
            if (out->positions[j] == out->positions[i]) {
                return fail(error, error_size,
                            "column '%s' is in the %s twice", name, kind);
            }
        }
    }

    return 1;
}

// keys are create's keys, resolved.
static int check_autoincrement(const CreateStatement *create,
                               const ColumnList *keys,
                               char *error, size_t error_size) {
    if (!create->has_autoincrement) {
        return 1;
    }

    size_t c = (size_t)create->autoincrement_column;
    const ColumnDef *column = &create->columns[c];

    if (column->type != TYPE_INT) {
        return fail(error, error_size,
                    "AUTOINCREMENT column '%s' must be INT", column->name);
    }

    for (int k = 0; k < create->key_count; k++) {
        if (create->keys[k].kind == CONSTRAINT_PRIMARY_KEY &&
            keys[k].count == 1 && keys[k].positions[0] == c) {
            return 1;
        }
    }
    return fail(error, error_size,
                "AUTOINCREMENT column '%s' must be the whole PRIMARY KEY",
                column->name);
}

static int same_columns(const ColumnList *a, const ColumnList *b) {
    if (a->count != b->count) {
        return 0;
    }
    for (size_t i = 0; i < a->count; i++) {
        if (!column_list_has(b, a->positions[i])) {
            return 0;
        }
    }
    return 1;
}

// e.g. "key (user_id, idempotency_key) is declared twice".
static int fail_same_key(const CreateStatement *create, const ColumnList *key,
                         char *error, size_t error_size) {
    char text[256];  // Cut short if it's long; it's only for the message.
    size_t length = 0;

    text[0] = '\0';
    for (size_t i = 0; i < key->count && length < sizeof(text); i++) {
        int written = snprintf(text + length, sizeof(text) - length, "%s%s",
                               i > 0 ? ", " : "",
                               create->columns[key->positions[i]].name);
        // snprintf returns the length it wanted, which may not have fit;
        // then length passes the end and the loop stops.
        length += written > 0 ? (size_t)written : 0;
    }

    return fail(error, error_size, "key (%s) is declared twice", text);
}

// Tables and indexes share one set of names, as in Postgres and SQLite.
static int check_name_free(const Database *db, const char *name,
                           char *error, size_t error_size) {
    size_t table;
    size_t index;

    if (database_lookup_table(db, name)) {
        return fail(error, error_size, "table '%s' already exists", name);
    }
    if (database_find_index(db, name, &table, &index)) {
        return fail(error, error_size, "index '%s' already exists", name);
    }
    return 1;
}

int execute_create(Database *db, const Statement *stmt,
                   char *error, size_t error_size) {
    const CreateStatement *create = &stmt->as.create;

    if (!check_name_free(db, stmt->table_name, error, error_size)) {
        return 0;
    }

    for (int i = 0; i < create->column_count; i++) {
        for (int j = 0; j < i; j++) {
            if (strcmp(create->columns[i].name,
                       create->columns[j].name) == 0) {
                return fail(error, error_size, "duplicate column '%s'",
                            create->columns[i].name);
            }
        }
    }

    ColumnList keys[MAX_KEYS];

    for (int i = 0; i < create->key_count; i++) {
        if (!resolve_key(create, &create->keys[i], &keys[i],
                         error, error_size)) {
            return 0;
        }
        for (int j = 0; j < i; j++) {
            if (same_columns(&keys[i], &keys[j])) {
                return fail_same_key(create, &keys[i], error, error_size);
            }
        }
    }

    if (!check_autoincrement(create, keys, error, error_size)) {
        return 0;
    }

    Table *table = database_create_table(db, stmt->table_name,
                                         create->columns,
                                         (size_t)create->column_count);
    if (!table) {
        return fail(error, error_size, "out of memory");
    }

    // Undo the half-made table, so a failure leaves no trace.
    for (int i = 0; i < create->key_count; i++) {
        if (!table_add_constraint(table, create->keys[i].kind, &keys[i])) {
            database_drop_table(db, table);
            return fail(error, error_size, "out of memory");
        }
    }

    table->autoincrement = create->has_autoincrement;

    return 1;
}

int execute_create_index(Database *db, const Statement *stmt,
                         char *error, size_t error_size) {
    const CreateIndexStatement *create = &stmt->as.create_index;
    Table *table = resolve_table(db, stmt->table_name, error, error_size);
    ColumnList columns;

    if (!table || !check_name_free(db, create->name, error, error_size)) {
        return 0;
    }
    if (table->index_count == MAX_INDEXES) {
        return fail(error, error_size, "too many indexes on table '%s' "
                    "(max %d)", table->name, MAX_INDEXES);
    }

    columns.count = (size_t)create->column_count;
    for (size_t i = 0; i < columns.count; i++) {
        const char *name = create->columns[i];

        if (!resolve_column(table, name, &columns.positions[i],
                            error, error_size)) {
            return 0;
        }
        for (size_t j = 0; j < i; j++) {
            if (columns.positions[j] == columns.positions[i]) {
                return fail(error, error_size,
                            "column '%s' is in the index twice", name);
            }
        }
    }

    if (!table_add_index(table, create->name, &columns)) {
        return fail(error, error_size, "out of memory");
    }
    return 1;
}

int execute_drop_index(Database *db, const Statement *stmt,
                       char *error, size_t error_size) {
    size_t table;
    size_t index;

    if (!database_find_index(db, stmt->index_name, &table, &index)) {
        return fail(error, error_size, "no such index '%s'",
                    stmt->index_name);
    }

    table_drop_index(&db->tables[table], index);
    return 1;
}

int execute_drop(Database *db, const Statement *stmt,
                 char *error, size_t error_size) {
    Table *table = resolve_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    database_drop_table(db, table);
    return 1;
}
