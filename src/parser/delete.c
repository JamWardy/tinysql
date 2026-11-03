#include "internal.h"

#include "text.h"

// delete_stmt = DELETE FROM IDENTIFIER [ WHERE condition ]
int parse_delete(Parser *p, Statement *out) {
    DeleteStatement *delete = &out->as.delete;

    if (!expect(p, TOKEN_DELETE, "DELETE") ||
        !expect(p, TOKEN_FROM, "FROM")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    if (!parse_where(p, &delete->has_where, &delete->where) ||
        !expect_end(p, delete->has_where ? "AND, OR or ';'"
                                         : "WHERE or ';'")) {
        return 0;
    }

    out->type = STMT_DELETE;
    copy_text(out->table_name, sizeof(out->table_name), name->text);
    return 1;
}
