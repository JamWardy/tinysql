#include "internal.h"

#include <stdlib.h>

#include "error.h"

// A matched row with its ORDER BY value, worked out before sorting.
typedef struct {
    const Row *row;
    Value key;  // Text points into the row or the statement.
} SortRow;

// qsort passes its comparison function only the two elements, so the
// direction is set here just before sorting.
static int sort_descending;

static int compare_rows(const void *a, const void *b) {
    const SortRow *x = a;
    const SortRow *y = b;
    int c = value_compare(&x->key, &y->key);

    if (sort_descending) {
        c = -c;
    }

    // qsort may reorder equal elements, so break ties by position in
    // the table (the rows are one array), keeping insertion order.
    if (c == 0) {
        c = (x->row > y->row) - (x->row < y->row);
    }
    return c;
}

// With no LIMIT, *limit is 0 and unused.
static int resolve_limit(const SelectStatement *select,
                         const ParamList *params, long long *limit,
                         char *error, size_t error_size) {
    *limit = 0;

    if (!select->has_limit) {
        return 1;
    }

    Value value = literal_to_value(&select->limit, params);

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

// Whether an item, or the ORDER BY, has an aggregate, so the SELECT gives one
// row for all it matches.
static int is_aggregate_select(const Statement *stmt) {
    const SelectStatement *select = &stmt->as.select;

    for (int i = 0; i < select->list.item_count; i++) {
        if (expr_has_aggregate(stmt->nodes, select->list.items[i].expr)) {
            return 1;
        }
    }
    return select->has_order &&
           expr_has_aggregate(stmt->nodes, select->order_by);
}

int select_resolve_list(ExprContext *ctx, const SelectList *list,
                        int allowed, const char *place,
                        char *error, size_t error_size) {
    for (int i = 0; i < list->item_count; i++) {
        if (!expr_resolve(ctx, list->items[i].expr, allowed, place,
                          error, error_size)) {
            return 0;
        }
    }
    return 1;
}

void select_add_columns(const ExprContext *ctx, const SelectList *list,
                        Result *result) {
    if (list->select_all) {
        for (size_t c = 0; c < ctx->table->column_count; c++) {
            const Column *column = &ctx->table->columns[c];
            result_add_column(result, column->name, column->type);
        }
        return;
    }

    for (int i = 0; i < list->item_count; i++) {
        const SelectItem *item = &list->items[i];
        const ExprNode *node = &ctx->nodes[item->expr];
        DataType type = ctx->types[item->expr];
        char heading[MAX_HEADING];

        if (item->alias[0] != '\0') {
            result_add_column(result, item->alias, type);
        } else if (node->kind == EXPR_COLUMN) {
            result_add_column(result, node->as.column, type);
        } else {
            expr_to_text(ctx->nodes, item->expr, heading, sizeof(heading));
            result_add_column(result, heading, type);
        }
    }
}

int select_append_row(const ExprContext *ctx, const SelectList *list,
                      const Row *row, Result *result,
                      char *error, size_t error_size) {
    Value values[MAX_COLUMNS];  // Borrowed from the row or the statement.
    const Value *row_values = values;

    if (list->select_all) {
        row_values = row->values;
    }

    for (int i = 0; i < list->item_count; i++) {
        if (!expr_evaluate(ctx, list->items[i].expr, row, &values[i],
                           error, error_size)) {
            return 0;
        }
    }

    if (!result_append_row(result, row_values)) {
        return fail(error, error_size, "out of memory");
    }
    return 1;
}

// Sorted by the ORDER BY if there is one; *count is how many to keep for the
// LIMIT. The caller frees *sorted.
static int sort_rows(const ExprContext *ctx, const SelectStatement *select,
                     long long limit, const RowList *rows,
                     SortRow **sorted, size_t *count,
                     char *error, size_t error_size) {
    *sorted = malloc(rows->count * sizeof(**sorted));
    *count = rows->count;

    // malloc(0) may return NULL, which isn't a failure.
    if (!*sorted && rows->count > 0) {
        return fail(error, error_size, "out of memory");
    }

    for (size_t r = 0; r < rows->count; r++) {
        (*sorted)[r].row = rows->rows[r];

        if (select->has_order &&
            !expr_evaluate(ctx, select->order_by, rows->rows[r],
                           &(*sorted)[r].key, error, error_size)) {
            free(*sorted);
            return 0;
        }
    }

    // Skipped for under two rows; *sorted may be NULL, which qsort
    // doesn't allow even with nothing to sort.
    if (select->has_order && rows->count > 1) {
        sort_descending = select->descending;
        qsort(*sorted, rows->count, sizeof(**sorted), compare_rows);
    }

    if (select->has_limit && (unsigned long long)limit < *count) {
        *count = (size_t)limit;
    }
    return 1;
}

static int select_rows(const ExprContext *ctx, const SelectStatement *select,
                       long long limit, const RowList *rows, Result *result,
                       char *error, size_t error_size) {
    SortRow *sorted;
    size_t count;

    if (!sort_rows(ctx, select, limit, rows, &sorted, &count,
                   error, error_size)) {
        return 0;
    }

    int ok = 1;

    for (size_t r = 0; r < count && ok; r++) {
        ok = select_append_row(ctx, &select->list, sorted[r].row, result,
                               error, error_size);
    }

    free(sorted);
    return ok;
}

// ORDER BY has nothing to sort, and LIMIT matters only when it's 0.
static int select_aggregates(ExprContext *ctx, const SelectStatement *select,
                             long long limit, const RowList *rows,
                             Result *result, char *error, size_t error_size) {
    for (int i = 0; i < select->list.item_count; i++) {
        if (!expr_compute_aggregates(ctx, select->list.items[i].expr, rows,
                                     error, error_size)) {
            return 0;
        }
    }

    if (select->has_limit && limit == 0) {
        return 1;
    }
    return select_append_row(ctx, &select->list, NULL, result,
                             error, error_size);
}

int execute_select(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size) {
    const SelectStatement *select = &stmt->as.select;
    const Table *table = resolve_table(db, stmt->table_name,
                                       error, error_size);

    if (!table) {
        return 0;
    }

    // Check every expression before reading any row, so a bad query
    // fails before any work is done. In an aggregate SELECT, columns
    // may only be read inside aggregates, as there's no GROUP BY.
    int aggregate = is_aggregate_select(stmt);
    int allowed = aggregate ? EXPR_AGGREGATES : EXPR_COLUMNS;
    ExprContext ctx;
    Filter filter;
    const Filter *where;
    long long limit;

    expr_init(&ctx, stmt, table, params);

    if (aggregate && select->list.select_all) {
        return fail(error, error_size,
                    "can't mix columns and aggregates (there's no GROUP BY)");
    }

    // An aggregate gives no table rows to lock.
    if (aggregate && select->for_update) {
        return fail(error, error_size,
                    "FOR UPDATE is not allowed with aggregates");
    }

    if (!select_resolve_list(&ctx, &select->list, allowed, "SELECT",
                             error, error_size)) {
        return 0;
    }

    if (!where_resolve(&ctx, select->has_where, select->where,
                       &filter, &where, error, error_size) ||
        (select->has_order &&
         !expr_resolve(&ctx, select->order_by, allowed, "ORDER BY",
                       error, error_size)) ||
        !resolve_limit(select, params, &limit, error, error_size)) {
        return 0;
    }

    RowList matched;

    if (!where_collect_rows(table, where, &matched, error, error_size)) {
        return 0;
    }

    select_add_columns(&ctx, &select->list, result);

    int ok = aggregate
                 ? select_aggregates(&ctx, select, limit, &matched, result,
                                     error, error_size)
                 : select_rows(&ctx, select, limit, &matched, result,
                               error, error_size);
    free(matched.rows);
    return ok;
}
