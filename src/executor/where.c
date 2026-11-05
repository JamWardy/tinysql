#include "internal.h"

#include <stdint.h>
#include <stdlib.h>

#include "error.h"

// A NULL filter matches every row. Otherwise a row matches when the
// WHERE is true for it; false and NULL (unknown) leave it out.
static int row_matches(const Row *row, const Filter *filter, int *matches,
                       Error *error) {
    Value value;

    if (!filter) {
        *matches = 1;
        return 1;
    }
    if (!expr_evaluate(filter->ctx, filter->root, row, &value, error)) {
        return 0;
    }
    *matches = value.type == TYPE_BOOL && value.as.boolean;
    return 1;
}

int where_resolve(ExprContext *ctx, int has_where, int root, Filter *filter,
                  const Filter **out, Error *error) {
    *out = NULL;

    if (!has_where) {
        return 1;
    }

    if (!expr_resolve(ctx, root, EXPR_COLUMNS, "WHERE", error)) {
        return 0;
    }

    DataType type = ctx->types[root];

    if (type != TYPE_BOOL && type != TYPE_NULL) {
        char text[64];
        expr_to_text(ctx->nodes, root, text, sizeof(text));
        return fail(error, ERROR_DATATYPE_MISMATCH,
                    "WHERE needs a BOOL, but %s is %s",
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

// Whether key's leading columns differ from the values the WHERE pins them to
// with =, in probe: then the walk has left those entries.
static int outside_equal_values(const Plan *plan, const Value *key,
                                const Value *probe) {
    for (size_t i = 0; i < plan->equal_count; i++) {
        if (value_compare(&key[i], &probe[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

// The ends of a plan's range on its next column, worked out as the walk
// starts: from the WHERE's comparisons, or from a LIKE's prefix.
typedef struct {
    int has_lower;
    int lower_inclusive;
    Value lower;
    int has_upper;
    int upper_inclusive;
    Value upper;
    LikeRange like;  // A LIKE's, whose text lower and upper point into; its
                     // lower is NULL otherwise.
} Bounds;

// Walking forwards: whether key is past plan's range, its range column NULL
// (which sorts last) or beyond the upper bound.
static int after_range(const Plan *plan, const Value *key, const Value *probe,
                       const Bounds *bounds) {
    size_t k = plan->equal_count;

    if (outside_equal_values(plan, key, probe)) {
        return 1;
    }
    if (!plan->range) {
        return 0;
    }
    if (key[k].type == TYPE_NULL) {
        return 1;
    }
    if (bounds->has_upper) {
        int c = value_compare(&key[k], &bounds->upper);
        return c > 0 || (c == 0 && !bounds->upper_inclusive);
    }
    return 0;
}

// Walking backwards: whether key is before plan's range, below the lower
// bound. NULLs, which come first this way, are left for the WHERE to turn
// away.
static int before_range(const Plan *plan, const Value *key, const Value *probe,
                        const Bounds *bounds) {
    size_t k = plan->equal_count;

    if (outside_equal_values(plan, key, probe)) {
        return 1;
    }
    if (!plan->range || !bounds->has_lower || key[k].type == TYPE_NULL) {
        return 0;
    }

    int c = value_compare(&key[k], &bounds->lower);
    return c < 0 || (c == 0 && !bounds->lower_inclusive);
}

// e reads no columns, so there's no row to give it. Sets *none if it's NULL, as
// then nothing can match.
static int evaluate_bound(const Filter *filter, int node, Value *out,
                          int *none, Error *error) {
    if (!expr_evaluate(filter->ctx, node, NULL, out, error)) {
        return 0;
    }
    if (out->type == TYPE_NULL) {
        *none = 1;
    }
    return 1;
}

static Value text_value(char *text) {
    Value value;
    value.type = TYPE_TEXT;
    value.as.text = text;
    return value;
}

// The bounds from the prefix of the LIKE at node. Sets *none if its pattern
// is NULL.
static int find_like_bounds(const Filter *filter, int node, Bounds *out,
                            int *none, Error *error) {
    LikePattern pattern;

    if (!expr_like_pattern(filter->ctx, node, NULL, &pattern, none, error)) {
        return 0;
    }
    if (*none) {
        return 1;
    }
    if (!like_range(&pattern, &out->like)) {
        return fail(error, ERROR_OUT_OF_MEMORY, "out of memory");
    }
    out->has_lower = 1;
    out->lower_inclusive = 1;
    out->lower = text_value(out->like.lower);
    if (out->like.upper) {
        out->has_upper = 1;
        out->upper_inclusive = out->like.upper_inclusive;
        out->upper = text_value(out->like.upper);
    }
    return 1;
}

// Sets *none if a bound is NULL. Free out with free_bounds, even on failure.
static int find_bounds(const Filter *filter, const Plan *plan, Bounds *out,
                       int *none, Error *error) {
    const IndexableConditions *range = plan->range;

    out->has_lower = 0;
    out->has_upper = 0;
    out->like.lower = NULL;

    if (!range) {
        return 1;
    }
    if (plan->like >= 0) {
        return find_like_bounds(filter, plan->like, out, none, error);
    }
    if (range->lower >= 0) {
        out->has_lower = 1;
        out->lower_inclusive = range->lower_inclusive;
        if (!evaluate_bound(filter, range->lower, &out->lower, none, error)) {
            return 0;
        }
    }
    if (range->upper >= 0) {
        out->has_upper = 1;
        out->upper_inclusive = range->upper_inclusive;
        if (!evaluate_bound(filter, range->upper, &out->upper, none, error)) {
            return 0;
        }
    }
    return 1;
}

static void free_bounds(Bounds *bounds) {
    if (bounds->like.lower) {
        like_range_free(&bounds->like);
    }
}

// Walks the range of plan's index from probe's pinned values and bounds,
// keeping the rows the whole WHERE matches, in index order if in_order and
// stopping once there are stop_after, or else in table order.
static int walk_index(const Table *table, const Filter *filter,
                      const Plan *plan, Value *probe, const Bounds *bounds,
                      int in_order, size_t stop_after, PositionList *out,
                      Error *error) {
    const TreeIndex *index = plan->index;
    size_t k = plan->equal_count;
    BTreeCursor cursor;

    // Start at the end of the range the walk begins from: the first entry
    // with the pinned columns, past the bound there if there is one.
    int backwards = plan->ordering == ORDERING_BACKWARDS;

    if (backwards) {
        if (bounds->has_upper) {
            probe[k] = bounds->upper;
            btree_seek_last(&index->tree, probe, k + 1,
                            bounds->upper_inclusive, &cursor);
        } else {
            btree_seek_last(&index->tree, probe, k, 1, &cursor);
        }
    } else if (bounds->has_lower) {
        probe[k] = bounds->lower;
        btree_seek_first(&index->tree, probe, k + 1, bounds->lower_inclusive,
                         &cursor);
    } else {
        btree_seek_first(&index->tree, probe, k, 1, &cursor);
    }

    while (btree_cursor_valid(&cursor) && out->count < stop_after) {
        const Value *key = btree_cursor_key(&cursor);
        size_t row = btree_cursor_row(&cursor);
        int matches;

        if (backwards ? before_range(plan, key, probe, bounds)
                      : after_range(plan, key, probe, bounds)) {
            break;
        }
        if (!row_matches(&table->rows[row], filter, &matches, error)) {
            return 0;
        }
        if (matches) {
            out->positions[out->count++] = row;
        }

        if (backwards) {
            btree_cursor_prev(&cursor);
        } else {
            btree_cursor_next(&cursor);
        }
    }

    // positions may be NULL with no rows, which qsort doesn't allow.
    if (!in_order && out->count > 1) {
        qsort(out->positions, out->count, sizeof(*out->positions),
              compare_positions);
    }
    return 1;
}

// Walks plan's range of its index, as walk_index does. out->positions has
// room for every row.
static int collect_from_index(const Table *table, const Filter *filter,
                              const CandidatePlans *candidates,
                              const Plan *plan, int in_order,
                              size_t stop_after, PositionList *out,
                              Error *error) {
    const TreeIndex *index = plan->index;
    Value probe[MAX_COLUMNS];
    Bounds bounds;
    int none = 0;

    for (size_t i = 0; i < plan->equal_count; i++) {
        int node = candidates->indexable[index->columns.positions[i]].equal;

        if (!evaluate_bound(filter, node, &probe[i], &none, error)) {
            return 0;
        }
    }

    int ok = find_bounds(filter, plan, &bounds, &none, error) &&
             (none || walk_index(table, filter, plan, probe, &bounds,
                                 in_order, stop_after, out, error));

    free_bounds(&bounds);
    return ok;
}

// A NULL finds nothing.
static int find_by_key(const Table *table, const Filter *filter,
                       const CandidatePlans *candidates, const Plan *plan,
                       size_t *row, int *found, Error *error) {
    const ColumnList *key = &plan->constraint->columns;
    Value values[MAX_COLUMNS];  // Laid out like a row; only the key's set.

    for (size_t i = 0; i < key->count; i++) {
        size_t column = key->positions[i];
        int none = 0;

        if (!evaluate_bound(filter, candidates->indexable[column].equal,
                            &values[column], &none, error)) {
            return 0;
        }
    }

    *found = table_find_key(table, plan->constraint, values, row);
    return 1;
}

// Fills out with the positions of the rows filter matches, by the plan
// choose_plan picks for order: in its order, setting *in_order, if the plan
// gives it, and then only up to its LIMIT; otherwise in table order.
static int collect(const Table *table, const Filter *filter,
                   const RowOrder *order, PositionList *out, int *in_order,
                   Error *error) {
    CandidatePlans candidates;

    choose_plan(table, filter, order, &candidates);

    const Plan *plan = &candidates.plans[candidates.chosen];
    size_t stop_after = SIZE_MAX;
    int ok = 1;

    *in_order = candidates.sorted && plan->ordering != ORDERING_NONE;
    if (*in_order && order->has_limit &&
        (unsigned long long)order->limit < table->row_count) {
        stop_after = (size_t)order->limit;
    }

    out->positions = malloc(table->row_count * sizeof(*out->positions));
    out->count = 0;

    // malloc(0) may return NULL, which isn't a failure.
    if (!out->positions && table->row_count > 0) {
        return fail(error, ERROR_OUT_OF_MEMORY, "out of memory");
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
                             error);
            if (ok && found) {
                out->positions[out->count++] = r;
            }
            break;
        }
        case PLAN_INDEX:
            // It checks the WHERE as it walks, to stop early.
            if (!collect_from_index(table, filter, &candidates, plan,
                                    *in_order, stop_after, out, error)) {
                free(out->positions);
                return 0;
            }
            return 1;
    }

    // Every plan finds possible rows; the whole WHERE decides which match.
    size_t kept = 0;
    int matches;

    for (size_t i = 0; i < out->count && ok; i++) {
        size_t r = out->positions[i];

        ok = row_matches(&table->rows[r], filter, &matches, error);
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

int where_collect_positions(const Table *table, const Filter *filter,
                            PositionList *out, Error *error) {
    int in_order;

    return collect(table, filter, NULL, out, &in_order, error);
}

int where_collect_rows(const Table *table, const Filter *filter,
                       const RowOrder *order, RowList *out, int *in_order,
                       Error *error) {
    PositionList matched;

    if (!collect(table, filter, order, &matched, in_order, error)) {
        return 0;
    }

    out->rows = malloc(matched.count * sizeof(*out->rows));
    if (!out->rows && matched.count > 0) {
        free(matched.positions);
        return fail(error, ERROR_OUT_OF_MEMORY, "out of memory");
    }

    for (size_t i = 0; i < matched.count; i++) {
        out->rows[i] = &table->rows[matched.positions[i]];
    }
    out->count = matched.count;

    free(matched.positions);
    return 1;
}
