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

// Sets equal_to[c] to the node of each `c = e`, or `e = c`, that the
// condition at node needs to be true, where e reads no columns and so
// is the same for every row: node itself, or a comparison reached from
// it through ANDs alone. Under an OR or NOT, no single comparison has to
// hold, so those are skipped.
static void find_required_equalities(const Filter *filter, int node,
                                     int *equal_to) {
    const ExprContext *ctx = filter->ctx;
    const ExprNode *n = &ctx->nodes[node];

    if (n->kind != EXPR_BINARY) {
        return;
    }

    if (n->as.op.op == OP_AND) {
        find_required_equalities(filter, n->as.op.left, equal_to);
        find_required_equalities(filter, n->as.op.right, equal_to);
        return;
    }

    if (n->as.op.op != OP_EQUAL) {
        return;
    }

    int left = n->as.op.left;
    int right = n->as.op.right;

    if (ctx->nodes[left].kind == EXPR_COLUMN &&
        !expr_uses_columns(ctx->nodes, right)) {
        equal_to[ctx->columns[left]] = right;
    } else if (ctx->nodes[right].kind == EXPR_COLUMN &&
               !expr_uses_columns(ctx->nodes, left)) {
        equal_to[ctx->columns[right]] = left;
    }
}

// The key a WHERE pins down: see find_key_lookup.
typedef struct {
    const Constraint *constraint;
    Value values[MAX_COLUMNS];  // Laid out like a row; only the key's set.
} KeyLookup;

// If filter needs `column = e` for every column of one of table's keys,
// as `WHERE id = 5` or `WHERE id = ? + 1 AND age > 30` do for the key
// (id), only the row with that key can match, and the key's index finds
// it without reading every row. Sets *found, and fills out if it's 1.
// Fails only if working out an e does.
static int find_key_lookup(const Table *table, const Filter *filter,
                           KeyLookup *out, int *found,
                           char *error, size_t error_size) {
    int equal_to[MAX_COLUMNS];

    for (size_t c = 0; c < MAX_COLUMNS; c++) {
        equal_to[c] = -1;
    }
    find_required_equalities(filter, filter->root, equal_to);

    *found = 0;

    for (size_t c = 0; c < table->constraint_count; c++) {
        const ColumnList *key = &table->constraints[c].columns;
        size_t i = 0;

        while (i < key->count && equal_to[key->positions[i]] >= 0) {
            i++;
        }
        if (i < key->count) {
            continue;
        }

        // It reads no columns, so there's no row to give it.
        for (i = 0; i < key->count; i++) {
            size_t column = key->positions[i];

            if (!expr_evaluate(filter->ctx, equal_to[column], NULL,
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

int where_collect_positions(const Table *table, const Filter *filter,
                            PositionList *out, char *error, size_t error_size) {
    KeyLookup lookup;
    int by_key = 0;

    if (filter && !find_key_lookup(table, filter, &lookup, &by_key,
                                   error, error_size)) {
        return 0;
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
        if (table_find_key(table, lookup.constraint, lookup.values, &r)) {
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
