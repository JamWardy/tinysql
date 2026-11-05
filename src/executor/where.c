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

// What a WHERE needs of one column, from comparisons `c op e` (or
// `e op c`) where e reads no columns, so is the same for every row.
typedef struct {
    int equal;            // e's node in `c = e`, or -1.
    int lower;            // e's node in `c > e` or `c >= e`, or -1.
    int lower_inclusive;  // 1 for >=.
    int upper;            // e's node in `c < e` or `c <= e`, or -1.
    int upper_inclusive;  // 1 for <=.
} ColumnBounds;

// Records in bounds each comparison the condition at node needs to be
// true: node itself, or one reached from it through ANDs alone. Under an
// OR or NOT, no single comparison has to hold, so those are skipped.
// Where a column has two of a kind, the last is kept; the whole WHERE is
// checked on every row found anyway.
static void find_bounds(const Filter *filter, int node, ColumnBounds *bounds) {
    const ExprContext *ctx = filter->ctx;
    const ExprNode *n = &ctx->nodes[node];

    if (n->kind != EXPR_BINARY) {
        return;
    }

    Operator op = n->as.op.op;
    int left = n->as.op.left;
    int right = n->as.op.right;

    if (op == OP_AND) {
        find_bounds(filter, left, bounds);
        find_bounds(filter, right, bounds);
        return;
    }

    int column;
    int value;

    if (ctx->nodes[left].kind == EXPR_COLUMN &&
        !expr_uses_columns(ctx->nodes, right)) {
        column = left;
        value = right;
    } else if (ctx->nodes[right].kind == EXPR_COLUMN &&
               !expr_uses_columns(ctx->nodes, left)) {
        column = right;
        value = left;

        // `5 < c` is `c > 5`.
        switch (op) {
            case OP_LESS:          op = OP_GREATER; break;
            case OP_LESS_EQUAL:    op = OP_GREATER_EQUAL; break;
            case OP_GREATER:       op = OP_LESS; break;
            case OP_GREATER_EQUAL: op = OP_LESS_EQUAL; break;
            default:               break;
        }
    } else {
        return;
    }

    ColumnBounds *b = &bounds[ctx->columns[column]];

    switch (op) {
        case OP_EQUAL:
            b->equal = value;
            break;
        case OP_GREATER:
        case OP_GREATER_EQUAL:
            b->lower = value;
            b->lower_inclusive = op == OP_GREATER_EQUAL;
            break;
        case OP_LESS:
        case OP_LESS_EQUAL:
            b->upper = value;
            b->upper_inclusive = op == OP_LESS_EQUAL;
            break;
        default:
            break;
    }
}

// The key a WHERE pins down: see find_key_lookup.
typedef struct {
    const Constraint *constraint;
    Value values[MAX_COLUMNS];  // Laid out like a row; only the key's set.
} KeyLookup;

// If bounds has `column = e` for every column of one of table's keys,
// as `WHERE id = 5` or `WHERE id = ? + 1 AND age > 30` do for the key
// (id), only the row with that key can match, and the key's index finds
// it without reading every row. Sets *found, and fills out if it's 1.
// Fails only if working out an e does.
static int find_key_lookup(const Table *table, const Filter *filter,
                           const ColumnBounds *bounds, KeyLookup *out,
                           int *found, char *error, size_t error_size) {
    *found = 0;

    for (size_t c = 0; c < table->constraint_count; c++) {
        const ColumnList *key = &table->constraints[c].columns;
        size_t i = 0;

        while (i < key->count && bounds[key->positions[i]].equal >= 0) {
            i++;
        }
        if (i < key->count) {
            continue;
        }

        // It reads no columns, so there's no row to give it.
        for (i = 0; i < key->count; i++) {
            size_t column = key->positions[i];

            if (!expr_evaluate(filter->ctx, bounds[column].equal, NULL,
                               &out->values[column], error, error_size)) {
                return 0;
            }
        }

        out->constraint = &table->constraints[c];
        *found = 1;
        return 1;
    }

    return 1;
}

// A B-tree index a WHERE can read a range of: see find_index_lookup.
typedef struct {
    const TreeIndex *index;
    size_t equal_count;  // Leading columns pinned by =.
    const ColumnBounds *range;  // The next column's, or NULL.
} IndexLookup;

// Picks the B-tree index with the most leading columns pinned by =,
// preferring one whose next column has a range, the first created on a
// tie. Sets *found, and fills out if it's 1.
static void find_index_lookup(const Table *table, const ColumnBounds *bounds,
                              IndexLookup *out, int *found) {
    *found = 0;

    for (size_t i = 0; i < table->index_count; i++) {
        const TreeIndex *index = &table->indexes[i];
        const ColumnBounds *range = NULL;
        size_t k = 0;

        while (k < index->columns.count &&
               bounds[index->columns.positions[k]].equal >= 0) {
            k++;
        }
        if (k < index->columns.count) {
            const ColumnBounds *b = &bounds[index->columns.positions[k]];
            if (b->lower >= 0 || b->upper >= 0) {
                range = b;
            }
        }
        if (k == 0 && !range) {
            continue;
        }

        if (!*found || k > out->equal_count ||
            (k == out->equal_count && range && !out->range)) {
            out->index = index;
            out->equal_count = k;
            out->range = range;
            *found = 1;
        }
    }
}

static int compare_positions(const void *a, const void *b) {
    size_t x = *(const size_t *)a;
    size_t y = *(const size_t *)b;

    return (x > y) - (x < y);
}

// Returns 1 if key, an index entry's, is past the end of lookup's range:
// its leading columns differ from probe's, or the range column is NULL
// (which sorts last) or beyond upper.
static int past_range(const IndexLookup *lookup, const Value *key,
                      const Value *probe, const Value *upper) {
    size_t k = lookup->equal_count;

    for (size_t i = 0; i < k; i++) {
        if (value_compare(&key[i], &probe[i]) != 0) {
            return 1;
        }
    }
    if (!lookup->range) {
        return 0;
    }
    if (key[k].type == TYPE_NULL) {
        return 1;
    }
    if (lookup->range->upper >= 0) {
        int c = value_compare(&key[k], upper);
        return c > 0 || (c == 0 && !lookup->range->upper_inclusive);
    }
    return 0;
}

// Sets *out to the value of e, the bound at node, which reads no
// columns, so there's no row to give it. Sets *none if it's NULL, as
// comparing with NULL is never true, so nothing can match.
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

// Fills out with the positions of the rows in lookup's range, in table
// order. out->positions has room for every row.
static int collect_from_index(const Filter *filter, const ColumnBounds *bounds,
                              const IndexLookup *lookup, PositionList *out,
                              char *error, size_t error_size) {
    const TreeIndex *index = lookup->index;
    const ColumnBounds *range = lookup->range;
    size_t k = lookup->equal_count;
    Value probe[MAX_COLUMNS];
    Value upper;
    int none = 0;
    BTreeCursor cursor;

    for (size_t i = 0; i < k; i++) {
        int node = bounds[index->columns.positions[i]].equal;

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
           !past_range(lookup, btree_cursor_key(&cursor), probe, &upper)) {
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

int where_collect_positions(const Table *table, const Filter *filter,
                            PositionList *out, char *error, size_t error_size) {
    ColumnBounds bounds[MAX_COLUMNS];
    KeyLookup key_lookup;
    IndexLookup index_lookup;
    int by_key = 0;
    int by_index = 0;

    for (size_t c = 0; c < MAX_COLUMNS; c++) {
        bounds[c].equal = -1;
        bounds[c].lower = -1;
        bounds[c].upper = -1;
    }

    if (filter) {
        find_bounds(filter, filter->root, bounds);
        if (!find_key_lookup(table, filter, bounds, &key_lookup, &by_key,
                             error, error_size)) {
            return 0;
        }
        if (!by_key) {
            find_index_lookup(table, bounds, &index_lookup, &by_index);
        }
    }

    size_t most = by_key ? 1 : table->row_count;

    out->positions = malloc(most * sizeof(*out->positions));
    out->count = 0;

    // malloc(0) may return NULL, which isn't a failure.
    if (!out->positions && most > 0) {
        return fail(error, error_size, "out of memory");
    }

    int matches;

    if (by_key) {
        size_t r;

        // The rest of the WHERE must hold too, as `age > 30` in
        // `WHERE id = 5 AND age > 30`. A key with a NULL finds nothing.
        if (table_find_key(table, key_lookup.constraint, key_lookup.values,
                           &r)) {
            if (!row_matches(&table->rows[r], filter, &matches,
                             error, error_size)) {
                free(out->positions);
                return 0;
            }
            if (matches) {
                out->positions[out->count++] = r;
            }
        }
        return 1;
    }

    if (by_index) {
        if (!collect_from_index(filter, bounds, &index_lookup, out,
                                error, error_size)) {
            free(out->positions);
            return 0;
        }

        // The rest of the WHERE must hold too, so keep only the matches.
        size_t kept = 0;
        for (size_t i = 0; i < out->count; i++) {
            size_t r = out->positions[i];

            if (!row_matches(&table->rows[r], filter, &matches,
                             error, error_size)) {
                free(out->positions);
                return 0;
            }
            if (matches) {
                out->positions[kept++] = r;
            }
        }
        out->count = kept;
        return 1;
    }

    for (size_t r = 0; r < table->row_count; r++) {
        if (!row_matches(&table->rows[r], filter, &matches,
                         error, error_size)) {
            free(out->positions);
            return 0;
        }
        if (matches) {
            out->positions[out->count++] = r;
        }
    }

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
