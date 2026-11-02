#include "internal.h"

#include <stdlib.h>

#include "error.h"

// SQL's three-valued logic. A comparison with NULL, such as `age > 30`
// when age is NULL, is neither true nor false but unknown: NULL means
// "no value", so there's nothing to compare. WHERE keeps a row only
// when its condition is true, so unknown rows are left out.
typedef enum {
    TRUTH_FALSE,
    TRUTH_TRUE,
    TRUTH_UNKNOWN
} Truth;

static Truth truth(int b) {
    return b ? TRUTH_TRUE : TRUTH_FALSE;
}

static Truth comparison_truth(const Value *value, const Comparison *cmp,
                              const ParamList *params) {
    if (cmp->op == CMP_IS_NULL) {
        return truth(value->type == TYPE_NULL);
    }
    if (cmp->op == CMP_IS_NOT_NULL) {
        return truth(value->type != TYPE_NULL);
    }
    if (value->type == TYPE_NULL) {
        return TRUTH_UNKNOWN;
    }

    // A `?` can be NULL, which compares as unknown, as above.
    Value wanted = common_literal_to_value(&cmp->value, params);
    if (wanted.type == TYPE_NULL) {
        return TRUTH_UNKNOWN;
    }

    int c = common_compare_values(value, &wanted);

    switch (cmp->op) {
        case CMP_EQUAL:         return truth(c == 0);
        case CMP_NOT_EQUAL:     return truth(c != 0);
        case CMP_LESS:          return truth(c < 0);
        case CMP_LESS_EQUAL:    return truth(c <= 0);
        case CMP_GREATER:       return truth(c > 0);
        case CMP_GREATER_EQUAL: return truth(c >= 0);
        case CMP_IS_NULL:
        case CMP_IS_NOT_NULL:
            break;  // Handled above.
    }
    return TRUTH_FALSE;
}

// Evaluates the condition node at index against row, recursing into
// its children. Unknown spreads only where it could change the answer:
// false AND unknown is false, and true OR unknown is true, whatever the
// unknown value turns out to be.
static Truth condition_truth(const Filter *filter, const Row *row,
                             int index) {
    const Condition *cond = &filter->conditions[index];

    switch (cond->type) {
        case COND_COMPARE:
            return comparison_truth(&row->values[filter->columns[index]],
                                    &cond->as.compare, filter->params);
        case COND_AND: {
            Truth left = condition_truth(filter, row, cond->as.children.left);
            if (left == TRUTH_FALSE) {
                return TRUTH_FALSE;
            }
            Truth right = condition_truth(filter, row,
                                          cond->as.children.right);
            if (right == TRUTH_FALSE) {
                return TRUTH_FALSE;
            }
            return left == TRUTH_TRUE && right == TRUTH_TRUE
                       ? TRUTH_TRUE : TRUTH_UNKNOWN;
        }
        case COND_OR: {
            Truth left = condition_truth(filter, row, cond->as.children.left);
            if (left == TRUTH_TRUE) {
                return TRUTH_TRUE;
            }
            Truth right = condition_truth(filter, row,
                                          cond->as.children.right);
            if (right == TRUTH_TRUE) {
                return TRUTH_TRUE;
            }
            return left == TRUTH_FALSE && right == TRUTH_FALSE
                       ? TRUTH_FALSE : TRUTH_UNKNOWN;
        }
        case COND_NOT:
            switch (condition_truth(filter, row, cond->as.children.left)) {
                case TRUTH_FALSE:   return TRUTH_TRUE;
                case TRUTH_TRUE:    return TRUTH_FALSE;
                case TRUTH_UNKNOWN: return TRUTH_UNKNOWN;
            }
    }
    return TRUTH_FALSE;
}

// A NULL filter matches every row.
static int row_matches(const Row *row, const Filter *filter) {
    return !filter ||
           condition_truth(filter, row, filter->root) == TRUTH_TRUE;
}

int where_resolve(const Table *table, const ParamList *params, int has_where,
                  const WhereClause *where, Filter *filter,
                  const Filter **out, char *error, size_t error_size) {
    *out = NULL;

    if (!has_where) {
        return 1;
    }

    filter->conditions = where->conditions;
    filter->root = where->root;
    filter->params = params;

    for (int i = 0; i < where->condition_count; i++) {
        if (where->conditions[i].type != COND_COMPARE) {
            continue;
        }

        const Comparison *cmp = &where->conditions[i].as.compare;
        size_t index;

        if (!common_find_column(table, cmp->column, &index,
                                error, error_size)) {
            return 0;
        }

        if (!common_check_type(&table->columns[index],
                               common_literal_type(&cmp->value, params),
                               "WHERE value", error, error_size)) {
            return 0;
        }

        filter->columns[i] = index;
    }

    *out = filter;
    return 1;
}

// Sets equal_to[c] to the value of each `c = value` that the condition
// at node needs to be true: node itself, or a comparison reached from it
// through ANDs alone. Under an OR or NOT, no single comparison has to
// hold, so those are skipped.
static void find_required_equalities(const Filter *filter, int node,
                                     const Literal **equal_to) {
    const Condition *cond = &filter->conditions[node];

    switch (cond->type) {
        case COND_COMPARE:
            if (cond->as.compare.op == CMP_EQUAL) {
                equal_to[filter->columns[node]] = &cond->as.compare.value;
            }
            break;
        case COND_AND:
            find_required_equalities(filter, cond->as.children.left,
                                     equal_to);
            find_required_equalities(filter, cond->as.children.right,
                                     equal_to);
            break;
        case COND_OR:
        case COND_NOT:
            break;
    }
}

// The key a WHERE pins down: see find_key_lookup.
typedef struct {
    const Constraint *constraint;
    Value values[MAX_COLUMNS];  // Laid out like a row; only the key's set.
} KeyLookup;

// If filter needs `column = value` for every column of one of table's
// keys, as `WHERE id = 5` or `WHERE id = 5 AND age > 30` do for the key
// (id), only the row with that key can match, and the key's index finds
// it without reading every row. Fills out and returns 1 if so.
static int find_key_lookup(const Table *table, const Filter *filter,
                           KeyLookup *out) {
    const Literal *equal_to[MAX_COLUMNS] = { NULL };

    find_required_equalities(filter, filter->root, equal_to);

    for (size_t c = 0; c < table->constraint_count; c++) {
        const ColumnList *key = &table->constraints[c].columns;
        size_t i = 0;

        while (i < key->count && equal_to[key->positions[i]]) {
            size_t column = key->positions[i];
            out->values[column] = common_literal_to_value(equal_to[column],
                                                          filter->params);
            i++;
        }

        if (i == key->count) {
            out->constraint = &table->constraints[c];
            return 1;
        }
    }

    return 0;
}

int where_collect_positions(const Table *table, const Filter *filter,
                            PositionList *out, char *error, size_t error_size) {
    KeyLookup lookup;
    int by_key = filter && find_key_lookup(table, filter, &lookup);
    size_t most = by_key ? 1 : table->row_count;

    out->positions = malloc(most * sizeof(*out->positions));
    out->count = 0;

    // malloc(0) may return NULL, which isn't a failure.
    if (!out->positions && most > 0) {
        return fail(error, error_size, "out of memory");
    }

    if (by_key) {
        size_t r;

        // The rest of the WHERE must hold too, as `age > 30` in
        // `WHERE id = 5 AND age > 30`.
        if (table_find_key(table, lookup.constraint, lookup.values, &r) &&
            row_matches(&table->rows[r], filter)) {
            out->positions[out->count++] = r;
        }
        return 1;
    }

    for (size_t r = 0; r < table->row_count; r++) {
        if (row_matches(&table->rows[r], filter)) {
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
