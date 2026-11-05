#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"

// What UPDATE sets: each column in columns gets the value of the
// expression with the same index in exprs. values holds the new values
// for the rows being updated, worked out before any row changes: for
// the i-th, values[i * columns.count + c] goes in column
// columns.positions[c]. Text there points into the old rows, the
// statement or a parameter.
typedef struct {
    ColumnList columns;
    int exprs[MAX_COLUMNS];  // Root nodes.
    Value *values;
} Changes;

// Fills after with row r's values as they'll be after the UPDATE: with
// the changes for the i-th updated row if updating[r] is i + 1, or as
// they are if it's 0. Text points into the rows or the statement, so
// after is only for reading straight away.
static void values_after_update(const Table *table, size_t r,
                                const size_t *updating,
                                const Changes *changes, Value *after) {
    const Row *row = &table->rows[r];

    memcpy(after, row->values, row->count * sizeof(*after));

    if (updating[r] > 0) {
        const Value *values = &changes->values[(updating[r] - 1) *
                                               changes->columns.count];

        for (size_t c = 0; c < changes->columns.count; c++) {
            after[changes->columns.positions[c]] = values[c];
        }
    }
}

// What check_key_after_update is looking for, passed through index_find.
typedef struct {
    const Table *table;
    const size_t *updating;
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

// Puts every row's new key in a new index, failing at the first already there.
// It takes every row, as two updated rows can clash with each other, not only
// with the rest. Runs only when an UPDATE sets a key column, which is rare.
static int check_key_after_update(const Table *table,
                                  const Constraint *constraint,
                                  const size_t *updating,
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

static int check_keys_after_update(const Table *table,
                                   const PositionList *matched,
                                   const Changes *changes,
                                   char *error, size_t error_size) {
    size_t *updating = NULL;  // Per row, as values_after_update reads it.
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
            updating = calloc(table->row_count, sizeof(*updating));
            if (!updating) {
                return fail(error, error_size, "out of memory");
            }
            for (size_t i = 0; i < matched->count; i++) {
                updating[matched->positions[i]] = i + 1;
            }
        }

        ok = check_key_after_update(table, constraint, updating, changes,
                                    error, error_size);
    }

    free(updating);
    return ok;
}

// Checks each column exists, is set at most once, and gets an expression of its
// type.
static int resolve_changes(ExprContext *ctx, const UpdateStatement *update,
                           Changes *out, char *error, size_t error_size) {
    const Table *table = ctx->table;

    out->columns.count = (size_t)update->assignment_count;
    out->values = NULL;

    for (size_t i = 0; i < out->columns.count; i++) {
        const Assignment *assign = &update->assignments[i];
        size_t index;

        if (!common_find_column(table, assign->column, &index,
                                error, error_size) ||
            !expr_resolve(ctx, assign->value, EXPR_COLUMNS, "SET",
                          error, error_size) ||
            !common_check_type(&table->columns[index],
                               ctx->types[assign->value], "value",
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
        out->exprs[i] = assign->value;
    }

    return 1;
}

// Fills changes->values with the new values for the rows in matched,
// each worked out from the row as it is. Fails, with nothing changed,
// on overflow, division by zero or a NULL for a NOT NULL column.
static int compute_changes(const ExprContext *ctx,
                           const PositionList *matched, Changes *changes,
                           char *error, size_t error_size) {
    const Table *table = ctx->table;
    size_t width = changes->columns.count;
    size_t count = matched->count * width;

    changes->values = malloc(count * sizeof(*changes->values));
    if (!changes->values && count > 0) {
        return fail(error, error_size, "out of memory");
    }

    for (size_t i = 0; i < matched->count; i++) {
        const Row *row = &table->rows[matched->positions[i]];

        for (size_t c = 0; c < width; c++) {
            Value *value = &changes->values[i * width + c];
            const Column *column = &table->columns[changes->columns.positions[c]];

            if (!expr_evaluate(ctx, changes->exprs[c], row, value,
                               error, error_size) ||
                !common_check_value(column, value->type, error, error_size)) {
                return 0;
            }
        }
    }

    return 1;
}

// Copied first, as they may point into the row itself: with SET a = b, b = a,
// writing a frees the text b's new value points to. If memory runs out, some
// columns may have changed.
static int write_row(Table *table, size_t r, const Changes *changes,
                     size_t i) {
    size_t width = changes->columns.count;
    Value copies[MAX_COLUMNS];
    int ok = 1;
    size_t copied = 0;

    while (copied < width && ok) {
        ok = value_copy(&copies[copied], &changes->values[i * width + copied]);
        copied += ok;
    }

    for (size_t c = 0; c < width && ok; c++) {
        ok = table_set_value(table, r, changes->columns.positions[c],
                             &copies[c]);
    }

    values_free(copies, copied);
    return ok;
}

int execute_update(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size) {
    const UpdateStatement *update = &stmt->as.update;
    Table *table = common_find_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    // Check every assignment, the WHERE and the keys, and work out every
    // new value, before changing any row.
    ExprContext ctx;
    Changes changes;
    Filter filter;
    const Filter *where;

    expr_init(&ctx, stmt, table, params);

    if (!resolve_changes(&ctx, update, &changes, error, error_size) ||
        !where_resolve(&ctx, update->has_where, update->where,
                       &filter, &where, error, error_size)) {
        return 0;
    }

    // Every row is tested before any changes, so a change to one row
    // can't affect the test on another.
    PositionList matched;

    if (!where_collect_positions(table, where, &matched, error, error_size)) {
        return 0;
    }

    if (!compute_changes(&ctx, &matched, &changes, error, error_size) ||
        !check_keys_after_update(table, &matched, &changes,
                                 error, error_size)) {
        free(changes.values);
        free(matched.positions);
        return 0;
    }

    size_t updated = 0;
    int ok = 1;

    // Only text needs memory. Running out partway leaves the changes
    // made so far in place; there's no undo.
    while (updated < matched.count && ok) {
        ok = write_row(table, matched.positions[updated], &changes, updated);
        updated += ok;
    }

    free(changes.values);
    free(matched.positions);

    if (!ok) {
        return fail(error, error_size,
                    "out of memory after updating %zu rows", updated);
    }
    result->changed = updated;
    return 1;
}
