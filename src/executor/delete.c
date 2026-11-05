#include "internal.h"

#include <stdlib.h>

int execute_delete(Database *db, const Statement *stmt,
                   const ParamList *params, Result *result,
                   char *error, size_t error_size) {
    const DeleteStatement *delete = &stmt->as.delete;
    Table *table = require_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    // As in SELECT, check the whole WHERE before touching any row, so a
    // bad column can't leave a delete half done.
    ExprContext ctx;
    Filter filter;
    const Filter *where;

    expr_init(&ctx, stmt, table, params);

    if (!where_resolve(&ctx, delete->has_where, delete->where,
                       &filter, &where, error, error_size)) {
        return 0;
    }

    PositionList matched;

    if (!where_collect_positions(table, where, &matched, error, error_size)) {
        return 0;
    }

    // From the last, so deleting a row doesn't move the ones still to go.
    for (size_t i = matched.count; i > 0; i--) {
        table_delete_row(table, matched.positions[i - 1]);
    }

    result->changed = matched.count;
    free(matched.positions);
    return 1;
}
