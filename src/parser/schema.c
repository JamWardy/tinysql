#include "internal.h"

#include "error.h"
#include "text.h"

// Adds a key of the given kind to create, with no columns yet. A table
// can have only one PRIMARY KEY. Returns the key, or NULL on failure.
static KeyDef *start_key(Parser *p, CreateStatement *create,
                         ConstraintKind kind) {
    if (kind == CONSTRAINT_PRIMARY_KEY) {
        for (int i = 0; i < create->key_count; i++) {
            if (create->keys[i].kind == CONSTRAINT_PRIMARY_KEY) {
                fail(p->error, sizeof(p->error),
                     "a table can have only one PRIMARY KEY");
                return NULL;
            }
        }
    }
    if (create->key_count == MAX_KEYS) {
        fail(p->error, sizeof(p->error), "too many keys (max %d)", MAX_KEYS);
        return NULL;
    }

    KeyDef *key = &create->keys[create->key_count++];
    key->kind = kind;
    key->column_count = 0;
    return key;
}

static int add_key_column(Parser *p, KeyDef *key, const char *name) {
    if (key->column_count == MAX_COLUMNS) {
        return fail(p->error, sizeof(p->error),
                    "too many %s columns (max %d)",
                    constraint_kind_name(key->kind), MAX_COLUMNS);
    }

    copy_text(key->columns[key->column_count],
              sizeof(key->columns[0]), name);
    key->column_count++;
    return 1;
}

// Adds a key of the given kind on the one column called name.
static int add_column_key(Parser *p, CreateStatement *create,
                          ConstraintKind kind, const char *name) {
    KeyDef *key = start_key(p, create, kind);

    return key && add_key_column(p, key, name);
}

// Adds a column to create.
//
// column_def        = IDENTIFIER column_type { column_constraint }
// column_type       = INT | TEXT
// column_constraint = NOT NULL | PRIMARY KEY | UNIQUE
static int parse_column_def(Parser *p, CreateStatement *create) {
    if (create->column_count == MAX_COLUMNS) {
        return fail(p->error, sizeof(p->error),
                    "too many columns (max %d)", MAX_COLUMNS);
    }

    ColumnDef *out = &create->columns[create->column_count];
    const Token *name = expect(p, TOKEN_IDENTIFIER, "column name");
    if (!name) {
        return 0;
    }
    copy_text(out->name, sizeof(out->name), name->text);

    if (match(p, TOKEN_INT)) {
        out->type = TYPE_INT;
    } else if (match(p, TOKEN_TEXT)) {
        out->type = TYPE_TEXT;
    } else {
        error_expected(p, "column type (INT or TEXT)");
        return 0;
    }

    out->not_null = 0;
    while (1) {
        if (match(p, TOKEN_NOT)) {
            if (!expect(p, TOKEN_NULL, "NULL")) {
                return 0;
            }
            out->not_null = 1;
        } else if (match(p, TOKEN_PRIMARY)) {
            if (!expect(p, TOKEN_KEY, "KEY") ||
                !add_column_key(p, create, CONSTRAINT_PRIMARY_KEY,
                                out->name)) {
                return 0;
            }
        } else if (match(p, TOKEN_UNIQUE)) {
            if (!add_column_key(p, create, CONSTRAINT_UNIQUE, out->name)) {
                return 0;
            }
        } else {
            break;
        }
    }

    create->column_count++;
    return 1;
}

// A key over one or more columns, after the columns or between them.
//
// table_key = ( PRIMARY KEY | UNIQUE )
//             LPAREN IDENTIFIER { COMMA IDENTIFIER } RPAREN
static int parse_table_key(Parser *p, CreateStatement *create) {
    ConstraintKind kind = CONSTRAINT_UNIQUE;

    if (match(p, TOKEN_PRIMARY)) {
        if (!expect(p, TOKEN_KEY, "KEY")) {
            return 0;
        }
        kind = CONSTRAINT_PRIMARY_KEY;
    } else if (!expect(p, TOKEN_UNIQUE, "PRIMARY or UNIQUE")) {
        return 0;
    }

    if (!expect(p, TOKEN_LPAREN, "'('")) {
        return 0;
    }

    KeyDef *key = start_key(p, create, kind);
    if (!key) {
        return 0;
    }

    do {
        const Token *column = expect(p, TOKEN_IDENTIFIER, "column name");
        if (!column || !add_key_column(p, key, column->text)) {
            return 0;
        }
    } while (match(p, TOKEN_COMMA));

    return expect(p, TOKEN_RPAREN, "',' or ')'") != NULL;
}

// create_stmt   = CREATE TABLE IDENTIFIER
//                 LPAREN table_element { COMMA table_element } RPAREN
// table_element = column_def | table_key
int parse_create(Parser *p, Statement *out) {
    if (!expect(p, TOKEN_CREATE, "CREATE") ||
        !expect(p, TOKEN_TABLE, "TABLE")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    if (!expect(p, TOKEN_LPAREN, "'('")) {
        return 0;
    }

    CreateStatement *create = &out->as.create;
    create->column_count = 0;
    create->key_count = 0;

    do {
        TokenType next = peek(p)->type;
        int ok = next == TOKEN_PRIMARY || next == TOKEN_UNIQUE
                     ? parse_table_key(p, create)
                     : parse_column_def(p, create);
        if (!ok) {
            return 0;
        }
    } while (match(p, TOKEN_COMMA));

    if (!expect(p, TOKEN_RPAREN, "',' or ')'")) {
        return 0;
    }

    // Only possible as `CREATE TABLE t (PRIMARY KEY (id))`, or UNIQUE.
    if (create->column_count == 0) {
        return fail(p->error, sizeof(p->error),
                    "a table needs at least one column");
    }

    out->type = STMT_CREATE;
    copy_text(out->table_name, sizeof(out->table_name), name->text);
    return 1;
}

// drop_stmt = DROP TABLE IDENTIFIER
int parse_drop(Parser *p, Statement *out) {
    if (!expect(p, TOKEN_DROP, "DROP") ||
        !expect(p, TOKEN_TABLE, "TABLE")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    out->type = STMT_DROP;
    copy_text(out->table_name, sizeof(out->table_name), name->text);
    return 1;
}
