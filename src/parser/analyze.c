#include "internal.h"

#include "text.h"

// analyze_stmt = ANALYZE [ IDENTIFIER ]
int parse_analyze(Parser *p, Statement *out) {
    if (!expect(p, TOKEN_ANALYZE, "ANALYZE")) {
        return 0;
    }

    out->type = STMT_ANALYZE;
    out->table_name[0] = '\0';
    if (peek(p)->type == TOKEN_IDENTIFIER) {
        copy_text(out->table_name, sizeof(out->table_name), peek(p)->text);
        match(p, TOKEN_IDENTIFIER);
    }
    return 1;
}
