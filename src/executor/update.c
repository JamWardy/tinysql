#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"

// The new value UPDATE gives each column it sets: values[i] goes in
// column columns.positions[i].
typedef struct {
    ColumnList columns;
    Value values[MAX_COLUMNS];
} Changes;

// Fills after with row r's values as they'll be after the UPDATE: with
// changes made if updating[r] is set, otherwise as they are. Text points
// into the row or the statement, so after is only for reading straight
// away.
static void values_after_update(const Table *table, size_t r,
                                const unsigned char *updating,
                                const Changes *changes, Value *after) {
    const Row *row = &table->rows[r];

    memcpy(after, row->values, row->count * sizeof(*after));

    if (updating[r]) {
        for (size_t i = 0; i < changes->columns.count; i++) {
            after[changes->columns.positions[i]] = changes->values[i];
        }
    }
}

// What check_key_after_update is looking for, passed through index_find.
typedef struct {
    const Table *table;
    const unsigned char *updating;
    const Changes *changes;
    const ColumnList *key;
    const Value *values;  // The key being looked for, laid out like a row.
} UpdatedKeySearch;

static int updated_row_has_key(size_t row, const void *context) {
    const UpdatedKeySearch *search = context;
    Value after[MAX_COLUMNS];

    values_after_update(search->table, row, search->updating,
                        search->changes, after);
    return key_equal(search->key, after, search->values);
}

// Checks that constraint's key would still be unique after the UPDATE,
// by putting every row's new key in a new index, one by one, and failing
// at the first that's there already. This reads every row, but only
// runs when an UPDATE sets a key column, which is rare.
//
// Checking only the updated rows against the table's index isn't
// enough: two updated rows can clash with each other.
static int check_key_after_update(const Table *table,
                                  const Constraint *constraint,
                                  const unsigned char *updating,
                                  const Changes *changes,
                                  char *error, size_t error_size) {
    Index seen;

    index_init(&seen);
    if (!index_reserve(&seen, table->row_count)) {
        return fail(error, error_size, "out of memory");
    }

    int ok = 1;

    for (size_t r = 0; r < table->row_count && ok; r++) {
        Value after[MAX_COLUMNS];
        values_after_update(table, r, updating, changes, after);

        // A key with a NULL can't clash with anything.
        if (key_has_null(&constraint->columns, after)) {
            continue;
        }

        uint64_t hash = key_hash(&constraint->columns, after);
        UpdatedKeySearch search = { table, updating, changes,
                                    &constraint->columns, after };
        size_t other;

        if (index_find(&seen, hash, updated_row_has_key, &search, &other)) {
            ok = common_fail_duplicate(table, constraint, after,
                                       error, error_size);
        } else {
            index_insert(&seen, hash, r);
        }
    }

    index_free(&seen);
    return ok;
}

// Checks that every key would still be unique after the UPDATE changes
// the rows in matched.
static int check_keys_after_update(const Table *table,
                                   const PositionList *matched,
                                   const Changes *changes,
                                   char *error, size_t error_size) {
    unsigned char *updating = NULL;  // Per row, 1 if it's being updated.
    int ok = 1;

    for (size_t c = 0; c < table->constraint_count && ok; c++) {
        const Constraint *constraint = &table->constraints[c];
        int sets_key = 0;

        for (size_t i = 0; i < changes->columns.count; i++) {
            if (column_list_has(&constraint->columns,
                                changes->columns.positions[i])) {
                sets_key = 1;
            }
        }

        // A key the UPDATE doesn't set, or sets in no rows, stays
        // unique.
        if (!sets_key || matched->count == 0) {
            continue;
        }

        if (!updating) {
            updating = calloc(table->row_count, 1);
            if (!updating) {
                return fail(error, error_size, "out of memory");
            }
            for (size_t i = 0; i < matched->count; i++) {
                updating[matched->positions[i]] = 1;
            }
        }

        ok = check_key_after_update(table, constraint, updating, changes,
                                    error, error_size);
    }

    free(updating);
    return ok;
}

// Fills out with the column and new value of each of update's
// assignments, checking each column exists, is set at most once, and can
// take its value.
static int resolve_changes(const Table *table, const UpdateStatement *update,
                           const ParamList *params, Changes *out,
                           char *error, size_t error_size) {
    out->columns.count = (size_t)update->assignment_count;

    for (size_t i = 0; i < out->columns.count; i++) {
        const Assignment *assign = &update->assignments[i];
        size_t index;

        if (!common_find_column(table, assign->column, &index,
                                error, error_size)) {
            return 0;
        }

        Value value = common_literal_to_value(&assign->value, params);

        if (!common_check_value(&table->columns[index], value.type,
                                error, error_size)) {
            return 0;
        }

        for (size_t j = 0; j < i; j++) {
            if (out->columns.positions[j] == index) {
                return fail(error, error_size, "column '%s' is set twice",
                            assign->column);
            }
        }

        out->columns.positions[i] = index;
        out->values[i] = value;
    }

    return 1;
}

int execute_update(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size) {
    const UpdateStatement *update = &stmt->as.update;
    Table *table = common_find_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    // Check every assignment, the WHERE and the keys before changing any
    // row.
    Changes changes;
    Filter filter;
    const Filter *where;

    if (!resolve_changes(table, update, params, &changes,
                         error, error_size) ||
        !where_resolve(table, params, update->has_where, &update->where,
                       &filter, &where, error, error_size)) {
        return 0;
    }

    // Every row is tested before any changes, so a change to one row
    // can't affect the test on another.
    PositionList matched;

    if (!where_collect_positions(table, where, &matched, error, error_size)) {
        return 0;
    }

    if (!check_keys_after_update(table, &matched, &changes,
                                 error, error_size)) {
        free(matched.positions);
        return 0;
    }

    size_t updated = 0;

    for (size_t i = 0; i < matched.count; i++) {
        for (size_t c = 0; c < changes.columns.count; c++) {
            // Only text needs memory. Running out partway leaves the
            // changes made so far in place; there's no undo.
            if (!table_set_value(table, matched.positions[i],
                                 changes.columns.positions[c],
                                 &changes.values[c])) {
                free(matched.positions);
                return fail(error, error_size,
                            "out of memory after updating %zu rows", updated);
            }
        }
        updated++;
    }

    result->changed = updated;
    free(matched.positions);
    return 1;
}
