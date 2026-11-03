#include "internal.h"

#include "error.h"

// Fills values, laid out like a row, from insert's expressions: in the
// table's order, or in the columns it names, with the others NULL.
// Checks each value suits its column, left-out ones included. Text in
// values points into the statement or a parameter.
// Returns 1, or 0 with a message in error.
static int fill_values(const Table *table, const Statement *stmt,
                       const ParamList *params, Value *values,
                       char *error, size_t error_size) {
    const InsertStatement *insert = &stmt->as.insert;
    int given[MAX_COLUMNS] = {0};
    ExprContext ctx;

    expr_init(&ctx, stmt, table, params);

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

            if (!common_find_column(table, name, &index, error, error_size)) {
                return 0;
            }
            if (given[index]) {
                return fail(error, error_size, "column '%s' is named twice",
                            name);
            }
        }

        // There's no row yet, so there are no columns to read.
        int root = insert->values[i];

        if (!expr_resolve(&ctx, root, 0, "VALUES", error, error_size) ||
            !common_check_type(&table->columns[index], ctx.types[root],
                               "value", error, error_size) ||
            !expr_evaluate(&ctx, root, NULL, &values[index],
                           error, error_size)) {
            return 0;
        }
        given[index] = 1;
    }

    for (size_t i = 0; i < table->column_count; i++) {
        if (!common_check_value(&table->columns[i], values[i].type,
                                error, error_size)) {
            return 0;
        }
    }
    return 1;
}

int execute_insert(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size) {
    Table *table = common_find_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    Value values[MAX_COLUMNS];

    if (!fill_values(table, stmt, params, values, error, error_size)) {
        return 0;
    }

    // Found through each key's index, so this doesn't read every row. A
    // key with a NULL is never found, as it can't clash.
    for (size_t i = 0; i < table->constraint_count; i++) {
        const Constraint *constraint = &table->constraints[i];
        size_t other;

        if (table_find_key(table, constraint, values, &other)) {
            return common_fail_duplicate(table, constraint, values,
                                         error, error_size);
        }
    }

    if (!table_append_row(table, values)) {
        return fail(error, error_size, "out of memory");
    }

    result->changed = 1;
    return 1;
}
