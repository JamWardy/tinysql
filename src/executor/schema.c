#include "internal.h"

#include <stdio.h>
#include <string.h>

#include "error.h"

// Fills out with the position of each of key's columns, checking that
// each is one of the new table's columns, and only named once.
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

// Checks the AUTOINCREMENT column, if there is one, is an INT and the
// whole PRIMARY KEY. keys are create's keys, resolved.
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

// Returns 1 if a and b hold the same columns, in any order.
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

// Fails with the message for a key declared twice, e.g.
// "key (user_id, idempotency_key) is declared twice".
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

int execute_create(Database *db, const Statement *stmt,
                   char *error, size_t error_size) {
    const CreateStatement *create = &stmt->as.create;

    if (database_find_table(db, stmt->table_name)) {
        return fail(error, error_size, "table '%s' already exists",
                    stmt->table_name);
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

int execute_drop(Database *db, const Statement *stmt,
                 char *error, size_t error_size) {
    Table *table = common_find_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    database_drop_table(db, table);
    return 1;
}
