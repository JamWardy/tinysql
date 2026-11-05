#include "internal.h"

#include "tinysql.h"  // For TINYSQL_MAX_ERROR.

// Planning: the ways a statement could find its rows, an estimate of
// how many rows each would read, a cost for each, and the cheapest.
// Estimates come from ANALYZE's statistics, or Postgres's default guesses
// without them.

// Fraction of rows guessed to match `x = y`, or anything else the planner can't
// estimate.
#define DEFAULT_EQUAL 0.005
// Fraction guessed to match one bound, `c > e` or `c <= e`.
#define DEFAULT_RANGE (1.0 / 3.0)
// Fraction guessed to be NULL.
#define DEFAULT_NULL 0.005

// Cost of reading a row in turn, and of reaching one through an index,
// as Postgres weighs reading in order against jumping about.
#define COST_READ 1.0
#define COST_INDEX 4.0

// Cost of comparing two rows while sorting: about Postgres's ratio of a
// comparison to handling a row.
#define COST_COMPARE 0.25

// Whether the condition at node compares a column with a value that reads no
// columns, either way round. If so, sets *column to the column's position,
// *op to the operator as if the column came first (`5 < c` is `c > 5`), and
// *value to the value's node.
static int column_comparison(const ExprContext *ctx, int node, size_t *column,
                             Operator *op, int *value) {
    const ExprNode *n = &ctx->nodes[node];

    if (n->kind != EXPR_BINARY) {
        return 0;
    }

    int left = n->as.op.left;
    int right = n->as.op.right;

    *op = n->as.op.op;
    if (ctx->nodes[left].kind == EXPR_COLUMN &&
        !expr_uses_columns(ctx->nodes, right)) {
        *column = ctx->columns[left];
        *value = right;
        return 1;
    }
    if (ctx->nodes[right].kind == EXPR_COLUMN &&
        !expr_uses_columns(ctx->nodes, left)) {
        *column = ctx->columns[right];
        *value = left;
        switch (*op) {
            case OP_LESS:          *op = OP_GREATER; break;
            case OP_LESS_EQUAL:    *op = OP_GREATER_EQUAL; break;
            case OP_GREATER:       *op = OP_LESS; break;
            case OP_GREATER_EQUAL: *op = OP_LESS_EQUAL; break;
            default:               break;
        }
        return 1;
    }
    return 0;
}

static void find_indexable_conditions(const Filter *filter, int node,
                                      IndexableConditions *indexable) {
    const ExprContext *ctx = filter->ctx;
    const ExprNode *n = &ctx->nodes[node];
    size_t column;
    Operator op;
    int value;

    if (n->kind == EXPR_BINARY && n->as.op.op == OP_AND) {
        find_indexable_conditions(filter, n->as.op.left, indexable);
        find_indexable_conditions(filter, n->as.op.right, indexable);
        return;
    }
    if (!column_comparison(ctx, node, &column, &op, &value)) {
        return;
    }

    IndexableConditions *b = &indexable[column];

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

static double default_selectivity(Operator op) {
    switch (op) {
        case OP_EQUAL:
            return DEFAULT_EQUAL;
        case OP_NOT_EQUAL:
            return 1.0 - DEFAULT_EQUAL;
        case OP_LESS:
        case OP_LESS_EQUAL:
        case OP_GREATER:
        case OP_GREATER_EQUAL:
            return DEFAULT_RANGE;
        case OP_NEGATE:
        case OP_NOT:
        case OP_ADD:
        case OP_SUBTRACT:
        case OP_MULTIPLY:
        case OP_DIVIDE:
        case OP_REMAINDER:
        case OP_AND:
        case OP_OR:
            break;
    }
    return DEFAULT_EQUAL;
}

// The value is either a common one, with its own fraction, or shares what's
// left evenly with the other values that aren't.
static double equal_selectivity(const ColumnStats *stats, const Value *value) {
    double rest = 1.0 - stats->null_fraction;
    double others = stats->distinct;

    for (size_t i = 0; i < stats->common_count; i++) {
        if (value_compare(&stats->common[i], value) == 0) {
            return stats->common_fraction[i];
        }
        rest -= stats->common_fraction[i];
        others -= 1.0;
    }
    return others > 0.0 && rest > 0.0 ? rest / others : 0.0;
}

// How far through the bucket from low to high value is, from 0 to 1. INTs by
// position; TEXT can't be placed so simply, so half way.
static double position_in_bucket(const Value *low, const Value *high,
                                 const Value *value) {
    if (value->type != TYPE_INT) {
        return 0.5;
    }

    double span = (double)high->as.integer - (double)low->as.integer;
    double into = (double)value->as.integer - (double)low->as.integer;

    return span > 0.0 ? into / span : 0.5;
}

// Fraction of the histogram's values that sort before value.
static double fraction_below(const ColumnStats *stats, const Value *value) {
    size_t last = stats->histogram_count - 1;

    if (value_compare(value, &stats->histogram[0]) < 0) {
        return 0.0;
    }
    if (value_compare(value, &stats->histogram[last]) >= 0) {
        return 1.0;
    }

    size_t i = 0;
    while (value_compare(value, &stats->histogram[i + 1]) >= 0) {
        i++;
    }
    return ((double)i + position_in_bucket(&stats->histogram[i],
                                           &stats->histogram[i + 1], value)) /
           (double)last;
}

static int holds(int compared, Operator op) {
    switch (op) {
        case OP_LESS:          return compared < 0;
        case OP_LESS_EQUAL:    return compared <= 0;
        case OP_GREATER:       return compared > 0;
        case OP_GREATER_EQUAL: return compared >= 0;
        default:               return 0;
    }
}

// The common values that satisfy op, plus the histogram's share of the rest.
static double range_selectivity(const ColumnStats *stats, const Value *value,
                                Operator op) {
    double result = 0.0;
    double rest = 1.0 - stats->null_fraction;

    for (size_t i = 0; i < stats->common_count; i++) {
        if (holds(value_compare(&stats->common[i], value), op)) {
            result += stats->common_fraction[i];
        }
        rest -= stats->common_fraction[i];
    }
    if (stats->histogram_count >= 2 && rest > 0.0) {
        double below = fraction_below(stats, value);
        int less = op == OP_LESS || op == OP_LESS_EQUAL;

        result += rest * (less ? below : 1.0 - below);
    }
    return result;
}

// `column op value`, from the column's statistics. Without them, or when the
// value can't be worked out (the statement will report why when it runs), the
// default guess.
static double comparison_selectivity(const ExprContext *ctx, size_t column,
                                     Operator op, int value_node) {
    const ColumnStats *stats =
        ctx->table->stats ? &ctx->table->stats[column] : NULL;
    char error[TINYSQL_MAX_ERROR];
    Value value;

    if (!stats || !expr_evaluate(ctx, value_node, NULL, &value,
                                 error, sizeof(error))) {
        return default_selectivity(op);
    }
    if (value.type == TYPE_NULL) {
        return 0.0;  // Comparing with NULL is never true.
    }

    double equal;

    switch (op) {
        case OP_EQUAL:
            return equal_selectivity(stats, &value);
        case OP_NOT_EQUAL:
            equal = equal_selectivity(stats, &value);
            return 1.0 - stats->null_fraction - equal > 0.0
                       ? 1.0 - stats->null_fraction - equal : 0.0;
        case OP_LESS:
        case OP_LESS_EQUAL:
        case OP_GREATER:
        case OP_GREATER_EQUAL:
            return range_selectivity(stats, &value, op);
        default:
            return default_selectivity(op);
    }
}

// From the statistics where a condition compares a column with a value or
// tests it for NULL; otherwise by Postgres's rules for when there are none.
static double estimate_selectivity(const ExprContext *ctx, int node) {
    const ExprNode *n = &ctx->nodes[node];
    size_t column;
    Operator op;
    int value;

    switch (n->kind) {
        case EXPR_LITERAL:
            // TRUE matches every row; FALSE and NULL none.
            return n->as.literal.kind == LITERAL_BOOL &&
                   n->as.literal.as.boolean ? 1.0 : 0.0;
        case EXPR_UNARY:
            if (n->as.op.op == OP_NOT) {
                return 1.0 - estimate_selectivity(ctx, n->as.op.left);
            }
            return DEFAULT_EQUAL;
        case EXPR_IS_NULL: {
            int operand = n->as.is_null.operand;
            double nulls = DEFAULT_NULL;

            if (ctx->nodes[operand].kind == EXPR_COLUMN && ctx->table->stats) {
                nulls = ctx->table->stats[ctx->columns[operand]].null_fraction;
            }
            return n->as.is_null.negated ? 1.0 - nulls : nulls;
        }
        case EXPR_BINARY:
            break;
        case EXPR_COLUMN:
        case EXPR_AGGREGATE:
            return DEFAULT_EQUAL;
    }

    double left;
    double right;

    switch (n->as.op.op) {
        case OP_AND:
            // Assumes the two are independent, as Postgres does.
            return estimate_selectivity(ctx, n->as.op.left) *
                   estimate_selectivity(ctx, n->as.op.right);
        case OP_OR:
            left = estimate_selectivity(ctx, n->as.op.left);
            right = estimate_selectivity(ctx, n->as.op.right);
            return left + right - left * right;
        default:
            break;
    }

    if (column_comparison(ctx, node, &column, &op, &value)) {
        return comparison_selectivity(ctx, column, op, value);
    }
    return default_selectivity(n->as.op.op);
}

// rows times fraction, but at least 1 if the table has any rows, as
// Postgres rounds estimates, so a guess never says a lookup is free.
static double estimate_rows(size_t rows, double fraction) {
    double estimate = (double)rows * fraction;

    if (rows == 0) {
        return 0.0;
    }
    return estimate < 1.0 ? 1.0 : estimate;
}

static void add_plan(CandidatePlans *candidates, PlanKind kind, double reads,
                     double cost_per_row) {
    Plan *plan = &candidates->plans[candidates->count++];

    plan->kind = kind;
    plan->constraint = NULL;
    plan->index = NULL;
    plan->equal_count = 0;
    plan->range = NULL;
    plan->ordering = ORDERING_NONE;
    plan->reads = reads;
    plan->cost = reads * cost_per_row;
}

// Each finds at most one row, through the key's hash index.
static void add_key_plans(const Table *table, CandidatePlans *candidates) {
    for (size_t c = 0; c < table->constraint_count; c++) {
        const ColumnList *key = &table->constraints[c].columns;
        size_t i = 0;

        while (i < key->count &&
               candidates->indexable[key->positions[i]].equal >= 0) {
            i++;
        }
        if (i == key->count) {
            add_plan(candidates, PLAN_KEY, table->row_count > 0 ? 1.0 : 0.0,
                     COST_INDEX);

            Plan *plan = &candidates->plans[candidates->count - 1];

            plan->constraint = &table->constraints[c];
            plan->ordering = ORDERING_FORWARDS;  // One row is in order.
        }
    }
}

// Whether walking index, with its first equal_count columns pinned, gives
// rows in order's order: its next column is the ORDER BY's, or the ORDER BY
// is on a pinned column, which then has one value.
static int index_gives_order(const TreeIndex *index, size_t equal_count,
                             const RowOrder *order) {
    if (!order || order->by != ORDER_BY_COLUMN) {
        return 0;
    }
    for (size_t i = 0; i <= equal_count && i < index->columns.count; i++) {
        if (index->columns.positions[i] == order->column) {
            return 1;
        }
    }
    return 0;
}

// For each index whose leading columns the WHERE pins with =, or with a range
// on its first unpinned column, or that gives the ORDER BY's order.
static void add_index_plans(const Table *table, const ExprContext *ctx,
                            const RowOrder *order,
                            CandidatePlans *candidates) {
    for (size_t i = 0; i < table->index_count; i++) {
        const TreeIndex *index = &table->indexes[i];
        const IndexableConditions *range = NULL;
        double fraction = 1.0;
        size_t k = 0;

        while (k < index->columns.count &&
               candidates->indexable[index->columns.positions[k]].equal >= 0) {
            size_t column = index->columns.positions[k];

            fraction *= comparison_selectivity(
                ctx, column, OP_EQUAL, candidates->indexable[column].equal);
            k++;
        }
        if (k < index->columns.count) {
            size_t column = index->columns.positions[k];
            const IndexableConditions *b = &candidates->indexable[column];

            if (b->lower >= 0) {
                fraction *= comparison_selectivity(
                    ctx, column,
                    b->lower_inclusive ? OP_GREATER_EQUAL : OP_GREATER,
                    b->lower);
            }
            if (b->upper >= 0) {
                fraction *= comparison_selectivity(
                    ctx, column, b->upper_inclusive ? OP_LESS_EQUAL : OP_LESS,
                    b->upper);
            }
            if (b->lower >= 0 || b->upper >= 0) {
                range = b;
            }
        }
        int gives_order = index_gives_order(index, k, order);

        if (k == 0 && !range && !gives_order) {
            continue;
        }

        add_plan(candidates, PLAN_INDEX,
                 estimate_rows(table->row_count, fraction),
                 COST_INDEX);
        Plan *plan = &candidates->plans[candidates->count - 1];
        plan->index = index;
        plan->equal_count = k;
        plan->range = range;

        // A pinned ORDER BY column has one value, so any direction will do.
        if (gives_order) {
            int on_next = k < index->columns.count &&
                          index->columns.positions[k] == order->column;

            plan->ordering = order->descending && on_next ? ORDERING_BACKWARDS
                                                          : ORDERING_FORWARDS;
        }
    }
}

// n log2 n comparisons, log2 rounded down.
static double sort_cost(double rows) {
    double log2 = 0.0;

    for (double n = rows; n >= 2.0; n /= 2.0) {
        log2 += 1.0;
    }
    return rows * log2 * COST_COMPARE;
}

// A plan giving rows in order needs no sort, and with a LIMIT stops once that
// many have matched: it reads the LIMIT's share of the rows it would.
// Any other plan reads them all, then sorts the matches.
static void apply_order(CandidatePlans *candidates, const RowOrder *order) {
    double sort = sort_cost(candidates->matches);

    for (size_t i = 0; i < candidates->count; i++) {
        Plan *plan = &candidates->plans[i];

        if (plan->ordering == ORDERING_NONE) {
            plan->cost += sort;
        } else if (order->has_limit &&
                   (double)order->limit < candidates->matches) {
            double share = (double)order->limit / candidates->matches;

            plan->reads *= share;
            plan->cost *= share;
        }
    }
}

void choose_plan(const Table *table, const Filter *filter,
                 const RowOrder *order, CandidatePlans *candidates) {
    for (size_t c = 0; c < MAX_COLUMNS; c++) {
        candidates->indexable[c].equal = -1;
        candidates->indexable[c].lower = -1;
        candidates->indexable[c].upper = -1;
    }
    candidates->count = 0;
    candidates->sorted = order && order->by != ORDER_BY_NOTHING;

    // Reading every row always works, so it's the one to beat.
    add_plan(candidates, PLAN_SCAN, (double)table->row_count, COST_READ);

    candidates->matches = (double)table->row_count;
    if (filter) {
        find_indexable_conditions(filter, filter->root, candidates->indexable);
        add_key_plans(table, candidates);

        double selectivity = estimate_selectivity(filter->ctx, filter->root);

        candidates->matches = estimate_rows(table->row_count, selectivity);
    }
    add_index_plans(table, filter ? filter->ctx : NULL,
                    candidates->sorted ? order : NULL, candidates);

    // A WHERE that pins down a whole key matches at most its one row.
    for (size_t i = 1; i < candidates->count; i++) {
        if (candidates->plans[i].kind == PLAN_KEY &&
            candidates->matches > candidates->plans[i].reads) {
            candidates->matches = candidates->plans[i].reads;
        }
    }

    if (candidates->sorted) {
        apply_order(candidates, order);
    }

    // The cheapest wins; on a tie, the first considered.
    candidates->chosen = 0;
    for (size_t i = 1; i < candidates->count; i++) {
        const Plan *best = &candidates->plans[candidates->chosen];

        if (candidates->plans[i].cost < best->cost) {
            candidates->chosen = i;
        }
    }
}
