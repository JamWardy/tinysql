#include "internal.h"

// Planning: the ways a statement could find its rows, an estimate of
// how many rows each would read, a cost for each, and the cheapest.
// Without statistics, estimates use Postgres's default guesses.

// Fraction of rows guessed to match `c = e`, and comparisons the
// planner can't estimate otherwise.
#define DEFAULT_EQUAL 0.005
// Fraction guessed to match one bound, `c > e` or `c <= e`.
#define DEFAULT_RANGE (1.0 / 3.0)
// Fraction guessed to be NULL.
#define DEFAULT_NULL 0.005

// Cost of reading a row in turn, and of reaching one through an index,
// as Postgres weighs reading in order against jumping about.
#define COST_READ 1.0
#define COST_INDEX 4.0

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

// Fraction of rows the condition at node is guessed to match, by
// Postgres's rules for when there are no statistics.
static double estimate(const ExprContext *ctx, int node) {
    const ExprNode *n = &ctx->nodes[node];

    switch (n->kind) {
        case EXPR_LITERAL:
            // TRUE matches every row; FALSE and NULL none.
            return n->as.literal.kind == LITERAL_BOOL &&
                   n->as.literal.as.boolean ? 1.0 : 0.0;
        case EXPR_UNARY:
            if (n->as.op.op == OP_NOT) {
                return 1.0 - estimate(ctx, n->as.op.left);
            }
            return DEFAULT_EQUAL;
        case EXPR_IS_NULL:
            return n->as.is_null.negated ? 1.0 - DEFAULT_NULL : DEFAULT_NULL;
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
            return estimate(ctx, n->as.op.left) *
                   estimate(ctx, n->as.op.right);
        case OP_OR:
            left = estimate(ctx, n->as.op.left);
            right = estimate(ctx, n->as.op.right);
            return left + right - left * right;
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
            break;
    }
    return DEFAULT_EQUAL;
}

// rows times fraction, but at least 1 if the table has any rows, as
// Postgres rounds estimates, so a guess never says a lookup is free.
static double some_rows(size_t rows, double fraction) {
    double estimate = (double)rows * fraction;

    if (rows == 0) {
        return 0.0;
    }
    return estimate < 1.0 ? 1.0 : estimate;
}

static void add_plan(PlanChoice *choice, PlanKind kind, double reads,
                     double cost_per_row) {
    Plan *plan = &choice->plans[choice->count++];

    plan->kind = kind;
    plan->constraint = NULL;
    plan->index = NULL;
    plan->equal_count = 0;
    plan->range = NULL;
    plan->reads = reads;
    plan->cost = reads * cost_per_row;
}

// Adds a plan for each key the WHERE pins down with =: it finds at most
// one row, through the key's hash index.
static void add_key_plans(const Table *table, PlanChoice *choice) {
    for (size_t c = 0; c < table->constraint_count; c++) {
        const ColumnList *key = &table->constraints[c].columns;
        size_t i = 0;

        while (i < key->count && choice->bounds[key->positions[i]].equal >= 0) {
            i++;
        }
        if (i == key->count) {
            add_plan(choice, PLAN_KEY, table->row_count > 0 ? 1.0 : 0.0,
                     COST_INDEX);
            choice->plans[choice->count - 1].constraint =
                &table->constraints[c];
        }
    }
}

// Adds a plan for each B-tree index whose leading columns the WHERE pins
// with =, or whose first unpinned column it bounds.
static void add_index_plans(const Table *table, PlanChoice *choice) {
    for (size_t i = 0; i < table->index_count; i++) {
        const TreeIndex *index = &table->indexes[i];
        const ColumnBounds *range = NULL;
        double fraction = 1.0;
        size_t k = 0;

        while (k < index->columns.count &&
               choice->bounds[index->columns.positions[k]].equal >= 0) {
            fraction *= DEFAULT_EQUAL;
            k++;
        }
        if (k < index->columns.count) {
            const ColumnBounds *b =
                &choice->bounds[index->columns.positions[k]];

            if (b->lower >= 0 || b->upper >= 0) {
                range = b;
                fraction *= b->lower >= 0 ? DEFAULT_RANGE : 1.0;
                fraction *= b->upper >= 0 ? DEFAULT_RANGE : 1.0;
            }
        }
        if (k == 0 && !range) {
            continue;
        }

        add_plan(choice, PLAN_INDEX, some_rows(table->row_count, fraction),
                 COST_INDEX);
        Plan *plan = &choice->plans[choice->count - 1];
        plan->index = index;
        plan->equal_count = k;
        plan->range = range;
    }
}

void plan_choose(const Table *table, const Filter *filter,
                 PlanChoice *choice) {
    for (size_t c = 0; c < MAX_COLUMNS; c++) {
        choice->bounds[c].equal = -1;
        choice->bounds[c].lower = -1;
        choice->bounds[c].upper = -1;
    }
    choice->count = 0;

    // Reading every row always works, so it's the one to beat.
    add_plan(choice, PLAN_SCAN, (double)table->row_count, COST_READ);

    choice->matches = (double)table->row_count;
    if (filter) {
        find_bounds(filter, filter->root, choice->bounds);
        add_key_plans(table, choice);
        add_index_plans(table, choice);
        choice->matches = some_rows(table->row_count,
                                    estimate(filter->ctx, filter->root));
    }

    // A WHERE that pins down a whole key matches at most its one row.
    for (size_t i = 1; i < choice->count; i++) {
        if (choice->plans[i].kind == PLAN_KEY &&
            choice->matches > choice->plans[i].reads) {
            choice->matches = choice->plans[i].reads;
        }
    }

    // The cheapest wins; on a tie, the first considered.
    choice->chosen = 0;
    for (size_t i = 1; i < choice->count; i++) {
        if (choice->plans[i].cost < choice->plans[choice->chosen].cost) {
            choice->chosen = i;
        }
    }
}
