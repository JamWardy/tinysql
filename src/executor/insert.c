#include "internal.h"

#include <limits.h>

#include "error.h"

// If the table has an AUTOINCREMENT column and values leaves it NULL,
// sets it to the ID after the highest the table has had.
static int choose_id(const Table *table, Value *values,
                     char *error, size_t error_size) {
    size_t c;

    if (!table_autoincrement_column(table, &c) ||
        values[c].type != TYPE_NULL) {
        return 1;
    }
    if (table->last_id == LLONG_MAX) {
        return fail(error, error_size, "table '%s' has run out of IDs",
                    table->name);
    }

    values[c].type = TYPE_INT;
    values[c].as.integer = table->last_id + 1;
    return 1;
}

// In the table's order, or in the columns insert names, with the others NULL.
// Checks each value given has its column's type. Text in values points into the
// statement or a parameter.
static int fill_values(ExprContext *ctx, const Statement *stmt,
                       Value *values, char *error, size_t error_size) {
    const InsertStatement *insert = &stmt->as.insert;
    const Table *table = ctx->table;
    int given[MAX_COLUMNS] = {0};

    if (insert->column_count == 0 &&
        (size_t)insert->value_count != table->column_count) {
        return fail(error, error_size,
                    "table '%s' has %zu columns but %d values were given",
                    table->name, table->column_count, insert->value_count);
    }

    for (size_t i = 0; i < table->column_count; i++) {
        values[i].type = TYPE_NULL;
    }

    for (int i = 0; i < insert->value_count; i++) {
        size_t index = (size_t)i;

        if (insert->column_count > 0) {
            const char *name = insert->columns[i];

            if (!resolve_column(table, name, &index, error, error_size)) {
                return 0;
            }
            if (given[index]) {
                return fail(error, error_size, "column '%s' is named twice",
                            name);
            }
        }

        // There's no row yet, so there are no columns to read.
        int root = insert->values[i];

        if (!expr_resolve(ctx, root, 0, "VALUES", error, error_size) ||
            !check_type(&table->columns[index], ctx->types[root],
                        "value", error, error_size) ||
            !expr_evaluate(ctx, root, NULL, &values[index],
                           error, error_size)) {
            return 0;
        }
        given[index] = 1;
    }
    return 1;
}

// Left-out ones included, so no NULL goes in a NOT NULL column.
static int check_values(const Table *table, const Value *values,
                        char *error, size_t error_size) {
    for (size_t i = 0; i < table->column_count; i++) {
        if (!check_value(&table->columns[i], values[i].type,
                         error, error_size)) {
            return 0;
        }
    }
    return 1;
}

int execute_insert(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size) {
    Table *table = resolve_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    const InsertStatement *insert = &stmt->as.insert;
    Value values[MAX_COLUMNS];
    ExprContext ctx;

    expr_init(&ctx, stmt, table, params);

    // An ID is chosen before the check, as the key can't be NULL.
    if (!fill_values(&ctx, stmt, values, error, error_size) ||
        !choose_id(table, values, error, error_size) ||
        !check_values(table, values, error, error_size)) {
        return 0;
    }

    // Found through each key's index, so this doesn't read every row. A
    // key with a NULL is never found, as it can't clash.
    for (size_t i = 0; i < table->constraint_count; i++) {
        const Constraint *constraint = &table->constraints[i];
        size_t other;

        if (table_find_key(table, constraint, values, &other)) {
            return fail_duplicate(table, constraint, values,
                                  error, error_size);
        }
    }

    // RETURNING reads the row before it goes in, so if evaluating it
    // fails, nothing has changed.
    if (insert->has_returning) {
        Row row = { values, table->column_count };

        if (!select_resolve_list(&ctx, &insert->returning, EXPR_COLUMNS,
                                 "RETURNING", error, error_size)) {
            return 0;
        }
        select_add_columns(&ctx, &insert->returning, result);
        if (!select_append_row(&ctx, &insert->returning, &row, result,
                               error, error_size)) {
            return 0;
        }
    }

    if (!table_append_row(table, values)) {
        return fail(error, error_size, "out of memory");
    }

    result->changed = 1;
    return 1;
}
