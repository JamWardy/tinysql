#include "internal.h"

#include <stdlib.h>

#include "error.h"

// A NULL filter matches every row. Otherwise a row matches when the
// WHERE is true for it; false and NULL (unknown) leave it out.
static int row_matches(const Row *row, const Filter *filter, int *matches,
                       char *error, size_t error_size) {
    Value value;

    if (!filter) {
        *matches = 1;
        return 1;
    }
    if (!expr_evaluate(filter->ctx, filter->root, row, &value,
                       error, error_size)) {
        return 0;
    }
    *matches = value.type == TYPE_BOOL && value.as.boolean;
    return 1;
}

int where_resolve(ExprContext *ctx, int has_where, int root, Filter *filter,
                  const Filter **out, char *error, size_t error_size) {
    *out = NULL;

    if (!has_where) {
        return 1;
    }

    if (!expr_resolve(ctx, root, EXPR_COLUMNS, "WHERE", error, error_size)) {
        return 0;
    }

    DataType type = ctx->types[root];

    if (type != TYPE_BOOL && type != TYPE_NULL) {
        char text[64];
        expr_to_text(ctx->nodes, root, text, sizeof(text));
        return fail(error, error_size, "WHERE needs a BOOL, but %s is %s",
                    text, data_type_name(type));
    }

    filter->ctx = ctx;
    filter->root = root;
    *out = filter;
    return 1;
}

static int compare_positions(const void *a, const void *b) {
    size_t x = *(const size_t *)a;
    size_t y = *(const size_t *)b;

    return (x > y) - (x < y);
}

// Returns 1 if key, an index entry's, is past the end of plan's range:
// its leading columns differ from probe's, or the range column is NULL
// (which sorts last) or beyond upper.
static int past_range(const Plan *plan, const Value *key,
                      const Value *probe, const Value *upper) {
    size_t k = plan->equal_count;

    for (size_t i = 0; i < k; i++) {
        if (value_compare(&key[i], &probe[i]) != 0) {
            return 1;
        }
    }
    if (!plan->range) {
        return 0;
    }
    if (key[k].type == TYPE_NULL) {
        return 1;
    }
    if (plan->range->upper >= 0) {
        int c = value_compare(&key[k], upper);
        return c > 0 || (c == 0 && !plan->range->upper_inclusive);
    }
    return 0;
}

// e reads no columns, so there's no row to give it. Sets *none if it's NULL, as
// then nothing can match.
static int evaluate_bound(const Filter *filter, int node, Value *out,
                          int *none, char *error, size_t error_size) {
    if (!expr_evaluate(filter->ctx, node, NULL, out, error, error_size)) {
        return 0;
    }
    if (out->type == TYPE_NULL) {
        *none = 1;
    }
    return 1;
}

// In table order. out->positions has room for every row.
static int collect_from_index(const Filter *filter,
                              const CandidatePlans *candidates,
                              const Plan *plan, PositionList *out,
                              char *error, size_t error_size) {
    const TreeIndex *index = plan->index;
    const IndexableConditions *range = plan->range;
    size_t k = plan->equal_count;
    Value probe[MAX_COLUMNS];
    Value upper;
    int none = 0;
    BTreeCursor cursor;

    for (size_t i = 0; i < k; i++) {
        int node = candidates->indexable[index->columns.positions[i]].equal;

        if (!evaluate_bound(filter, node, &probe[i], &none,
                            error, error_size)) {
            return 0;
        }
    }
    if (range && range->lower >= 0 &&
        !evaluate_bound(filter, range->lower, &probe[k], &none,
                        error, error_size)) {
        return 0;
    }
    if (range && range->upper >= 0 &&
        !evaluate_bound(filter, range->upper, &upper, &none,
                        error, error_size)) {
        return 0;
    }
    if (none) {
        return 1;
    }

    // Start at the first entry with the pinned columns, past the lower
    // bound if there is one.
    if (range && range->lower >= 0) {
        btree_seek_first(&index->tree, probe, k + 1, range->lower_inclusive,
                         &cursor);
    } else {
        btree_seek_first(&index->tree, probe, k, 1, &cursor);
    }

    while (btree_cursor_valid(&cursor) &&
           !past_range(plan, btree_cursor_key(&cursor), probe, &upper)) {
        out->positions[out->count++] = btree_cursor_row(&cursor);
        btree_cursor_next(&cursor);
    }

    // The index gives rows in key order; callers expect table order.
    // positions may be NULL with no rows, which qsort doesn't allow.
    if (out->count > 1) {
        qsort(out->positions, out->count, sizeof(*out->positions),
              compare_positions);
    }
    return 1;
}

// A NULL finds nothing.
static int find_by_key(const Table *table, const Filter *filter,
                       const CandidatePlans *candidates, const Plan *plan,
                       size_t *row, int *found,
                       char *error, size_t error_size) {
    const ColumnList *key = &plan->constraint->columns;
    Value values[MAX_COLUMNS];  // Laid out like a row; only the key's set.

    for (size_t i = 0; i < key->count; i++) {
        size_t column = key->positions[i];
        int none = 0;

        if (!evaluate_bound(filter, candidates->indexable[column].equal,
                            &values[column], &none, error, error_size)) {
            return 0;
        }
    }

    *found = table_find_key(table, plan->constraint, values, row);
    return 1;
}

int where_collect_positions(const Table *table, const Filter *filter,
                            PositionList *out, char *error, size_t error_size) {
    CandidatePlans candidates;

    choose_plan(table, filter, &candidates);

    const Plan *plan = &candidates.plans[candidates.chosen];
    int ok = 1;

    out->positions = malloc(table->row_count * sizeof(*out->positions));
    out->count = 0;

    // malloc(0) may return NULL, which isn't a failure.
    if (!out->positions && table->row_count > 0) {
        return fail(error, error_size, "out of memory");
    }

    switch (plan->kind) {
        case PLAN_SCAN:
            for (size_t r = 0; r < table->row_count; r++) {
                out->positions[out->count++] = r;
            }
            break;
        case PLAN_KEY: {
            size_t r;
            int found;

            ok = find_by_key(table, filter, &candidates, plan, &r, &found,
                             error, error_size);
            if (ok && found) {
                out->positions[out->count++] = r;
            }
            break;
        }
        case PLAN_INDEX:
            ok = collect_from_index(filter, &candidates, plan, out,
                                    error, error_size);
            break;
    }

    // Every plan finds possible rows; the whole WHERE decides which match.
    size_t kept = 0;
    int matches;

    for (size_t i = 0; i < out->count && ok; i++) {
        size_t r = out->positions[i];

        ok = row_matches(&table->rows[r], filter, &matches,
                         error, error_size);
        if (ok && matches) {
            out->positions[kept++] = r;
        }
    }

    if (!ok) {
        free(out->positions);
        return 0;
    }
    out->count = kept;
    return 1;
}

int where_collect_rows(const Table *table, const Filter *filter, RowList *out,
                       char *error, size_t error_size) {
    PositionList matched;

    if (!where_collect_positions(table, filter, &matched, error, error_size)) {
        return 0;
    }

    out->rows = malloc(matched.count * sizeof(*out->rows));
    if (!out->rows && matched.count > 0) {
        free(matched.positions);
        return fail(error, error_size, "out of memory");
    }

    for (size_t i = 0; i < matched.count; i++) {
        out->rows[i] = &table->rows[matched.positions[i]];
    }
    out->count = matched.count;

    free(matched.positions);
    return 1;
}
