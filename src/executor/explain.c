#include "internal.h"

#include <stdio.h>

#include "error.h"

// Only SELECT, UPDATE and DELETE have one.
static void find_where(const Statement *stmt, int *has_where, int *where) {
    *has_where = 0;
    *where = -1;

    switch (stmt->type) {
        case STMT_SELECT:
            *has_where = stmt->as.select.has_where;
            *where = stmt->as.select.where;
            break;
        case STMT_UPDATE:
            *has_where = stmt->as.update.has_where;
            *where = stmt->as.update.where;
            break;
        case STMT_DELETE:
            *has_where = stmt->as.delete.has_where;
            *where = stmt->as.delete.where;
            break;
        case STMT_CREATE:
        case STMT_INSERT:
        case STMT_DROP:
        case STMT_CREATE_INDEX:
        case STMT_DROP_INDEX:
        case STMT_BEGIN:
        case STMT_COMMIT:
        case STMT_ROLLBACK:
            break;  // The parser allows EXPLAIN on none of these.
    }
}

// Writes a range's operators, e.g. ">" or ">= and <".
static void write_range(char *out, size_t size, const IndexableConditions *b) {
    const char *lower = b->lower_inclusive ? ">=" : ">";
    const char *upper = b->upper_inclusive ? "<=" : "<";

    if (b->lower >= 0 && b->upper >= 0) {
        snprintf(out, size, "%s and %s", lower, upper);
    } else {
        snprintf(out, size, "%s", b->lower >= 0 ? lower : upper);
    }
}

// e.g. "every row", "PRIMARY KEY (id)" or "index orders_item_expires (item_id
// =, expires_at >)".
static void describe(const Table *table, const Plan *plan, char *out,
                     size_t size) {
    const ColumnList *columns;
    size_t used;
    size_t shown;

    switch (plan->kind) {
        case PLAN_SCAN:
            snprintf(out, size, "every row");
            return;
        case PLAN_KEY:
            columns = &plan->constraint->columns;
            shown = columns->count;
            used = (size_t)snprintf(out, size, "%s (",
                                    constraint_kind_name(
                                        plan->constraint->kind));
            break;
        case PLAN_INDEX:
            columns = &plan->index->columns;
            shown = plan->equal_count + (plan->range ? 1 : 0);
            used = (size_t)snprintf(out, size, "index %s (",
                                    plan->index->name);
            break;
    }

    for (size_t i = 0; i < shown && used < size; i++) {
        const char *name = table->columns[columns->positions[i]].name;
        char ops[16] = "=";

        if (plan->kind == PLAN_INDEX && i == plan->equal_count) {
            write_range(ops, sizeof(ops), plan->range);
        }
        if (plan->kind == PLAN_KEY) {
            ops[0] = '\0';  // A key is pinned whole, by =.
        }
        used += (size_t)snprintf(out + used, size - used, "%s%s%s%s",
                                 i > 0 ? ", " : "", name,
                                 ops[0] ? " " : "", ops);
    }
    if (used < size) {
        snprintf(out + used, size - used, ")");
    }
}

static long long round_estimate(double estimate) {
    return (long long)(estimate + 0.5);
}

int execute_explain(Database *db, const Statement *stmt,
                    const ParamList *params, Result *result,
                    char *error, size_t error_size) {
    Table *table = require_table(db, stmt->table_name, error, error_size);
    ExprContext ctx;
    Filter filter;
    const Filter *where;
    CandidatePlans candidates;
    int has_where;
    int root;

    if (!table) {
        return 0;
    }

    find_where(stmt, &has_where, &root);
    expr_init(&ctx, stmt, table, params);
    if (!where_resolve(&ctx, has_where, root, &filter, &where,
                       error, error_size)) {
        return 0;
    }

    choose_plan(table, where, &candidates);

    result_add_column(result, "plan", TYPE_TEXT);
    result_add_column(result, "reads", TYPE_INT);
    result_add_column(result, "cost", TYPE_INT);
    result_add_column(result, "chosen", TYPE_TEXT);
    result_add_column(result, "matches", TYPE_INT);

    for (size_t i = 0; i < candidates.count; i++) {
        const Plan *plan = &candidates.plans[i];
        char text[256];
        Value row[5];

        describe(table, plan, text, sizeof(text));
        row[0].type = TYPE_TEXT;
        row[0].as.text = text;
        row[1].type = TYPE_INT;
        row[1].as.integer = round_estimate(plan->reads);
        row[2].type = TYPE_INT;
        row[2].as.integer = round_estimate(plan->cost);
        row[3].type = TYPE_TEXT;
        row[3].as.text = i == candidates.chosen ? "yes" : "";
        row[4].type = TYPE_INT;
        row[4].as.integer = round_estimate(candidates.matches);

        if (!result_append_row(result, row)) {
            return fail(error, error_size, "out of memory");
        }
    }
    return 1;
}
