#include "internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "error.h"

// qsort passes its comparison function only the two elements, so the
// ORDER BY column and direction are set here just before sorting.
static struct {
    size_t column;
    int descending;
} sort_by;

// common_compare_values for sorting, where either value may be NULL. NULL
// sorts after every other value, as in PostgreSQL, so NULLs come last
// in ascending order and first in descending order.
static int compare_for_sort(const Value *a, const Value *b) {
    int a_null = a->type == TYPE_NULL;
    int b_null = b->type == TYPE_NULL;

    if (a_null || b_null) {
        return a_null - b_null;
    }
    return common_compare_values(a, b);
}

// Compares two elements of an array of const Row *.
static int compare_rows(const void *a, const void *b) {
    const Row *x = *(const Row *const *)a;
    const Row *y = *(const Row *const *)b;
    int c = compare_for_sort(&x->values[sort_by.column],
                             &y->values[sort_by.column]);

    if (sort_by.descending) {
        c = -c;
    }

    // qsort may reorder equal elements, so break ties by position in
    // the table (the rows are one array), keeping insertion order.
    if (c == 0) {
        c = (x > y) - (x < y);
    }
    return c;
}

// Writes the column heading for agg into heading, e.g. "COUNT(*)" or
// "MAX(age)".
static void aggregate_heading(const Aggregate *agg, char *heading,
                              size_t size) {
    snprintf(heading, size, "%s(%s)", aggregate_name(agg->func),
             agg->column[0] == '\0' ? "*" : agg->column);
}

// Counts rows for COUNT(*), or for COUNT(column) the rows whose value
// at index column isn't NULL.
static long long count_values(const Aggregate *agg, size_t column,
                              const RowList *rows) {
    if (agg->column[0] == '\0') {
        return (long long)rows->count;
    }

    long long count = 0;
    for (size_t r = 0; r < rows->count; r++) {
        if (rows->rows[r]->values[column].type != TYPE_NULL) {
            count++;
        }
    }
    return count;
}

// Sets *out to agg over rows, reading the table column at index column
// (unused for COUNT(*)). Text in *out points into a row, so copy it
// before the table changes.
//
// COUNT(*) counts rows, NULLs and all, and COUNT(column) the non-NULL
// values; both give 0 when there are none. SUM, MIN and MAX skip NULLs,
// and give NULL when there's no other value: no rows, or only NULLs.
static int compute_aggregate(const Aggregate *agg, size_t column,
                             const RowList *rows, Value *out,
                             char *error, size_t error_size) {
    if (agg->func == AGG_COUNT) {
        out->type = TYPE_INT;
        out->as.integer = count_values(agg, column, rows);
        return 1;
    }

    out->type = TYPE_NULL;

    for (size_t r = 0; r < rows->count; r++) {
        const Value *value = &rows->rows[r]->values[column];

        if (value->type == TYPE_NULL) {
            continue;
        }

        // The first value found starts the result.
        if (out->type == TYPE_NULL) {
            *out = *value;
            continue;
        }

        switch (agg->func) {
            case AGG_SUM: {
                long long x = value->as.integer;
                long long sum = out->as.integer;

                // Check before adding: in C, overflowing a signed
                // integer is undefined behaviour, not a wrap-around.
                if ((x > 0 && sum > LLONG_MAX - x) ||
                    (x < 0 && sum < LLONG_MIN - x)) {
                    return fail(error, error_size, "SUM(%s) is out of range",
                                agg->column);
                }
                out->as.integer = sum + x;
                break;
            }
            case AGG_MIN:
                if (common_compare_values(value, out) < 0) {
                    *out = *value;
                }
                break;
            case AGG_MAX:
                if (common_compare_values(value, out) > 0) {
                    *out = *value;
                }
                break;
            case AGG_COUNT:
                break;  // Handled above.
        }
    }

    return 1;
}

// Fills result with select's aggregates over rows as a single row,
// under headings such as "COUNT(*)". columns holds the table column each
// aggregate reads, as filled by resolve_aggregates. There's only one
// row, so ORDER BY has nothing to sort and LIMIT matters only when it's
// 0: then the result has its columns but no row.
static int aggregate_result(const Table *table,
                            const SelectStatement *select, long long limit,
                            const ColumnList *columns, const RowList *rows,
                            Result *result, char *error, size_t error_size) {
    Value values[MAX_COLUMNS];

    for (size_t i = 0; i < columns->count; i++) {
        const Aggregate *agg = &select->aggregates[i];
        char heading[MAX_HEADING];

        if (!compute_aggregate(agg, columns->positions[i], rows, &values[i],
                               error, error_size)) {
            return 0;
        }

        // COUNT gives an INT whatever it counts; the others give a value
        // from their column, or NULL.
        DataType type = agg->func == AGG_COUNT
                            ? TYPE_INT
                            : table->columns[columns->positions[i]].type;

        aggregate_heading(agg, heading, sizeof(heading));
        result_add_column(result, heading, type);
    }

    int has_row = !select->has_limit || limit > 0;

    if (has_row && !result_append_row(result, values)) {
        return fail(error, error_size, "out of memory");
    }
    return 1;
}

// Fills out with the table column for each selected column: all of
// them, in order, for SELECT *.
static int resolve_columns(const Table *table, const SelectStatement *select,
                           ColumnList *out, char *error, size_t error_size) {
    if (select->select_all) {
        out->count = table->column_count;
        for (size_t i = 0; i < out->count; i++) {
            out->positions[i] = i;
        }
        return 1;
    }

    out->count = (size_t)select->column_count;
    for (size_t i = 0; i < out->count; i++) {
        if (!common_find_column(table, select->columns[i], &out->positions[i],
                                error, error_size)) {
            return 0;
        }
    }

    return 1;
}

// Fills out with the table column each aggregate reads, checking that
// SUM is given an INT column.
static int resolve_aggregates(const Table *table,
                              const SelectStatement *select,
                              ColumnList *out, char *error, size_t error_size) {
    out->count = (size_t)select->aggregate_count;

    for (size_t i = 0; i < out->count; i++) {
        const Aggregate *agg = &select->aggregates[i];

        if (agg->column[0] == '\0') {
            // COUNT(*) reads no column; 0 is a placeholder.
            out->positions[i] = 0;
            continue;
        }

        if (!common_find_column(table, agg->column, &out->positions[i],
                                error, error_size)) {
            return 0;
        }

        const Column *column = &table->columns[out->positions[i]];
        if (agg->func == AGG_SUM && column->type != TYPE_INT) {
            return fail(error, error_size,
                        "SUM needs an INT column but '%s' is %s",
                        column->name, data_type_name(column->type));
        }
    }

    return 1;
}

// Sets *order_column to the ORDER BY column. With no ORDER BY it's set
// to 0 and unused: select->has_order says whether there is one.
static int resolve_order_by(const Table *table,
                            const SelectStatement *select,
                            size_t *order_column,
                            char *error, size_t error_size) {
    *order_column = 0;

    if (!select->has_order) {
        return 1;
    }

    return common_find_column(table, select->order_column, order_column,
                              error, error_size);
}

// Sets *limit to the LIMIT, which a `?` may give. With no LIMIT it's set
// to 0 and unused: select->has_limit says whether there is one.
static int resolve_limit(const SelectStatement *select,
                         const ParamList *params, long long *limit,
                         char *error, size_t error_size) {
    *limit = 0;

    if (!select->has_limit) {
        return 1;
    }

    Value value = common_literal_to_value(&select->limit, params);

    if (value.type != TYPE_INT) {
        return fail(error, error_size,
                    "LIMIT needs an INT but the value is %s",
                    data_type_name(value.type));
    }
    if (value.as.integer < 0) {
        return fail(error, error_size, "LIMIT can't be negative: %lld",
                    value.as.integer);
    }

    *limit = value.as.integer;
    return 1;
}

// Sorts rows in place by the ORDER BY column, if there is one, and
// cuts rows->count to the LIMIT, limit.
static void sort_and_limit(const SelectStatement *select,
                           size_t order_column, long long limit,
                           RowList *rows) {
    // Skipped for under two rows; rows->rows may be NULL, which qsort
    // doesn't allow even with nothing to sort.
    if (select->has_order && rows->count > 1) {
        sort_by.column = order_column;
        sort_by.descending = select->descending;
        qsort(rows->rows, rows->count, sizeof(*rows->rows), compare_rows);
    }

    if (select->has_limit && (unsigned long long)limit < rows->count) {
        rows->count = (size_t)limit;
    }
}

// Fills result with the chosen columns of rows, in order, under the
// table's column names.
static int rows_result(const Table *table, const ColumnList *columns,
                       const RowList *rows, Result *result,
                       char *error, size_t error_size) {
    for (size_t i = 0; i < columns->count; i++) {
        const Column *column = &table->columns[columns->positions[i]];
        result_add_column(result, column->name, column->type);
    }

    for (size_t r = 0; r < rows->count; r++) {
        Value values[MAX_COLUMNS];  // Borrowed from the row, not copies.

        for (size_t i = 0; i < columns->count; i++) {
            values[i] = rows->rows[r]->values[columns->positions[i]];
        }

        if (!result_append_row(result, values)) {
            return fail(error, error_size, "out of memory");
        }
    }
    return 1;
}

// SELECT of columns, e.g. SELECT name, age FROM people WHERE age > 30.
static int select_rows(const Table *table, const Statement *stmt,
                       const ParamList *params, Result *result,
                       char *error, size_t error_size) {
    const SelectStatement *select = &stmt->as.select;
    ExprContext ctx;

    expr_init(&ctx, stmt, table, params);

    // Check every name before reading any row, so a bad query fails
    // before any work is done.
    ColumnList columns;
    Filter filter;
    const Filter *where;
    size_t order_column;
    long long limit;

    if (!resolve_columns(table, select, &columns, error, error_size) ||
        !where_resolve(&ctx, select->has_where, select->where,
                       &filter, &where, error, error_size) ||
        !resolve_order_by(table, select, &order_column,
                          error, error_size) ||
        !resolve_limit(select, params, &limit, error, error_size)) {
        return 0;
    }

    RowList matched;

    if (!where_collect_rows(table, where, &matched, error, error_size)) {
        return 0;
    }

    sort_and_limit(select, order_column, limit, &matched);

    int ok = rows_result(table, &columns, &matched, result,
                         error, error_size);
    free(matched.rows);
    return ok;
}

// SELECT of aggregates, e.g. SELECT COUNT(*), MAX(age) FROM people.
static int select_aggregates(const Table *table, const Statement *stmt,
                             const ParamList *params, Result *result,
                             char *error, size_t error_size) {
    const SelectStatement *select = &stmt->as.select;
    ExprContext ctx;

    expr_init(&ctx, stmt, table, params);

    ColumnList columns;  // The column each aggregate reads.
    Filter filter;
    const Filter *where;
    size_t order_column;  // Unused: one result row has nothing to sort.
    long long limit;

    // ORDER BY is still resolved, so a bad column name is an error here
    // as it is everywhere else.
    if (!resolve_aggregates(table, select, &columns, error, error_size) ||
        !where_resolve(&ctx, select->has_where, select->where,
                       &filter, &where, error, error_size) ||
        !resolve_order_by(table, select, &order_column,
                          error, error_size) ||
        !resolve_limit(select, params, &limit, error, error_size)) {
        return 0;
    }

    RowList matched;

    if (!where_collect_rows(table, where, &matched, error, error_size)) {
        return 0;
    }

    int ok = aggregate_result(table, select, limit, &columns, &matched,
                              result, error, error_size);
    free(matched.rows);
    return ok;
}

int execute_select(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size) {
    const SelectStatement *select = &stmt->as.select;
    const Table *table = common_find_table(db, stmt->table_name,
                                           error, error_size);

    if (!table) {
        return 0;
    }

    return select->has_aggregates
               ? select_aggregates(table, stmt, params, result,
                                   error, error_size)
               : select_rows(table, stmt, params, result,
                             error, error_size);
}
