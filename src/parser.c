#include "parser.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_ERROR 256

// The parser's working state while it reads one statement. It lives only
// for the duration of a parse; the result goes into the caller's Statement.
typedef struct {
    const TokenList *tokens;  // Input, read-only.
    int pos;                  // Index of the next token to read.
    char error[MAX_ERROR];    // Set by the first check that fails.
} Parser;

// Returns the current token without consuming it.
static const Token *peek(const Parser *p) {
    // The tokeniser guarantees the list ends in EOF, and nothing consumes
    // EOF, so pos can only run past the end if the parser has a bug.
    assert(p->pos < p->tokens->count);
    return &p->tokens->tokens[p->pos];
}

// Records "expected <what> but found ..." for the current token.
static void error_expected(Parser *p, const char *what) {
    const Token *token = peek(p);

    if (token->type == TOKEN_EOF) {
        snprintf(p->error, sizeof(p->error),
                 "expected %s but reached end of input", what);
    } else {
        snprintf(p->error, sizeof(p->error),
                 "expected %s but found '%s'", what, token->text);
    }
}

// Consumes the current token if it has the given type and returns it.
// Otherwise records an error naming what was expected and returns NULL.
static const Token *expect(Parser *p, TokenType type, const char *what) {
    const Token *token = peek(p);

    if (token->type != type) {
        error_expected(p, what);
        return NULL;
    }

    p->pos++;
    return token;
}

// Consumes the current token if it has the given type. Unlike expect,
// a mismatch isn't an error: it's used where the grammar has a choice.
static int match(Parser *p, TokenType type) {
    if (peek(p)->type != type) {
        return 0;
    }

    p->pos++;
    return 1;
}

// The only places a Literal is built, so its tag always matches its value.
static Literal literal_int(long long n) {
    Literal lit;
    lit.type = TYPE_INT;
    lit.as.integer = n;
    return lit;
}

static Literal literal_text(const char *text) {
    Literal lit;
    lit.type = TYPE_TEXT;
    snprintf(lit.as.text, sizeof(lit.as.text), "%s", text);
    return lit;
}

long long literal_as_int(const Literal *lit) {
    assert(lit->type == TYPE_INT);
    return lit->as.integer;
}

const char *literal_as_text(const Literal *lit) {
    assert(lit->type == TYPE_TEXT);
    return lit->as.text;
}

// column_def = IDENTIFIER column_type
// column_type = INT | TEXT
static int parse_column_def(Parser *p, ColumnDef *out) {
    const Token *name = expect(p, TOKEN_IDENTIFIER, "column name");
    if (!name) {
        return 0;
    }

    if (match(p, TOKEN_INT)) {
        out->type = TYPE_INT;
    } else if (match(p, TOKEN_TEXT)) {
        out->type = TYPE_TEXT;
    } else {
        error_expected(p, "column type (INT or TEXT)");
        return 0;
    }

    snprintf(out->name, sizeof(out->name), "%s", name->text);
    return 1;
}

// create_stmt = CREATE TABLE IDENTIFIER
//               LPAREN column_def { COMMA column_def } RPAREN
static int parse_create(Parser *p, Statement *out) {
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

    do {
        if (create->column_count == MAX_COLUMNS) {
            snprintf(p->error, sizeof(p->error),
                     "too many columns (max %d)", MAX_COLUMNS);
            return 0;
        }

        if (!parse_column_def(p, &create->columns[create->column_count])) {
            return 0;
        }

        create->column_count++;
    } while (match(p, TOKEN_COMMA));

    if (!expect(p, TOKEN_RPAREN, "',' or ')'")) {
        return 0;
    }

    out->type = STMT_CREATE;
    snprintf(out->table_name, sizeof(out->table_name), "%s", name->text);
    return 1;
}

// value = [ MINUS ] NUMBER | STRING
static int parse_value(Parser *p, Literal *out) {
    int negative = match(p, TOKEN_MINUS);
    const Token *token = peek(p);

    if (token->type == TOKEN_NUMBER) {
        // Convert the sign and digits together rather than negating
        // afterwards: the most negative long long has no positive
        // counterpart, so it can't be read as positive first.
        char text[MAX_TOKEN_LENGTH + 1];
        snprintf(text, sizeof(text), "%s%s", negative ? "-" : "",
                 token->text);

        // The tokeniser only accepts digits, so the whole text converts;
        // the remaining failure is a number too big for a long long.
        errno = 0;
        long long n = strtoll(text, NULL, 10);

        if (errno == ERANGE) {
            snprintf(p->error, sizeof(p->error),
                     "number out of range: %s", text);
            return 0;
        }

        *out = literal_int(n);
    } else if (token->type == TOKEN_STRING && !negative) {
        *out = literal_text(token->text);
    } else {
        error_expected(p, negative ? "number after '-'"
                                   : "value (number or 'text')");
        return 0;
    }

    p->pos++;
    return 1;
}

// insert_stmt = INSERT INTO IDENTIFIER
//               VALUES LPAREN value { COMMA value } RPAREN
static int parse_insert(Parser *p, Statement *out) {
    if (!expect(p, TOKEN_INSERT, "INSERT") ||
        !expect(p, TOKEN_INTO, "INTO")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    if (!expect(p, TOKEN_VALUES, "VALUES") ||
        !expect(p, TOKEN_LPAREN, "'('")) {
        return 0;
    }

    InsertStatement *insert = &out->as.insert;
    insert->value_count = 0;

    do {
        if (insert->value_count == MAX_COLUMNS) {
            snprintf(p->error, sizeof(p->error),
                     "too many values (max %d)", MAX_COLUMNS);
            return 0;
        }

        if (!parse_value(p, &insert->values[insert->value_count])) {
            return 0;
        }

        insert->value_count++;
    } while (match(p, TOKEN_COMMA));

    if (!expect(p, TOKEN_RPAREN, "',' or ')'")) {
        return 0;
    }

    out->type = STMT_INSERT;
    snprintf(out->table_name, sizeof(out->table_name), "%s", name->text);
    return 1;
}

// select_list = STAR | IDENTIFIER { COMMA IDENTIFIER }
static int parse_select_list(Parser *p, SelectStatement *out) {
    out->column_count = 0;

    if (match(p, TOKEN_STAR)) {
        out->select_all = 1;
        return 1;
    }

    out->select_all = 0;

    do {
        if (out->column_count == MAX_COLUMNS) {
            snprintf(p->error, sizeof(p->error),
                     "too many columns (max %d)", MAX_COLUMNS);
            return 0;
        }

        const Token *column = expect(
            p, TOKEN_IDENTIFIER,
            out->column_count == 0 ? "'*' or column name" : "column name");
        if (!column) {
            return 0;
        }

        snprintf(out->columns[out->column_count],
                 sizeof(out->columns[out->column_count]),
                 "%s", column->text);
        out->column_count++;
    } while (match(p, TOKEN_COMMA));

    return 1;
}

// comparison = IDENTIFIER compare_op value
// compare_op = EQUAL | NOT_EQUAL | LESS | LESS_EQUAL
//            | GREATER | GREATER_EQUAL
static int parse_comparison(Parser *p, Comparison *out) {
    const Token *column = expect(p, TOKEN_IDENTIFIER,
                                 "column name, NOT or '('");
    if (!column) {
        return 0;
    }

    switch (peek(p)->type) {
        case TOKEN_EQUAL:         out->op = CMP_EQUAL; break;
        case TOKEN_NOT_EQUAL:     out->op = CMP_NOT_EQUAL; break;
        case TOKEN_LESS:          out->op = CMP_LESS; break;
        case TOKEN_LESS_EQUAL:    out->op = CMP_LESS_EQUAL; break;
        case TOKEN_GREATER:       out->op = CMP_GREATER; break;
        case TOKEN_GREATER_EQUAL: out->op = CMP_GREATER_EQUAL; break;

        default:
            error_expected(p, "comparison (=, !=, <, <=, > or >=)");
            return 0;
    }
    p->pos++;

    if (!parse_value(p, &out->value)) {
        return 0;
    }

    snprintf(out->column, sizeof(out->column), "%s", column->text);
    return 1;
}

// Claims the next free condition node and sets its type. Children are
// claimed before their parent, so a parent's index is always higher.
static int new_condition(Parser *p, WhereClause *where,
                         ConditionType type, int *out) {
    if (where->condition_count == MAX_CONDITIONS) {
        snprintf(p->error, sizeof(p->error),
                 "WHERE is too complex (max %d conditions)", MAX_CONDITIONS);
        return 0;
    }

    *out = where->condition_count++;
    where->conditions[*out].type = type;
    return 1;
}

// Claims an AND or OR node joining left and right.
static int join_conditions(Parser *p, WhereClause *where,
                           ConditionType type, int left, int right,
                           int *out) {
    if (!new_condition(p, where, type, out)) {
        return 0;
    }

    where->conditions[*out].as.children.left = left;
    where->conditions[*out].as.children.right = right;
    return 1;
}

// The condition rules call each other, from loosest to tightest binding.
// Each one fills *out with the index of the node it built.
static int parse_condition(Parser *p, WhereClause *where, int *out);

// not_condition = NOT not_condition
//               | LPAREN condition RPAREN
//               | comparison
static int parse_not_condition(Parser *p, WhereClause *where,
                               int *out) {
    if (match(p, TOKEN_NOT)) {
        int operand;

        if (!parse_not_condition(p, where, &operand) ||
            !new_condition(p, where, COND_NOT, out)) {
            return 0;
        }

        where->conditions[*out].as.children.left = operand;
        return 1;
    }

    if (match(p, TOKEN_LPAREN)) {
        return parse_condition(p, where, out) &&
               expect(p, TOKEN_RPAREN, "AND, OR or ')'") != NULL;
    }

    return new_condition(p, where, COND_COMPARE, out) &&
           parse_comparison(p, &where->conditions[*out].as.compare);
}

// and_condition = not_condition { AND not_condition }
static int parse_and_condition(Parser *p, WhereClause *where,
                               int *out) {
    if (!parse_not_condition(p, where, out)) {
        return 0;
    }

    // Each AND takes everything so far as its left side, so
    // `a AND b AND c` groups as `(a AND b) AND c`.
    while (match(p, TOKEN_AND)) {
        int right;

        if (!parse_not_condition(p, where, &right) ||
            !join_conditions(p, where, COND_AND, *out, right, out)) {
            return 0;
        }
    }

    return 1;
}

// condition = and_condition { OR and_condition }
//
// OR is parsed outermost, so AND binds tighter:
// `a OR b AND c` means `a OR (b AND c)`.
static int parse_condition(Parser *p, WhereClause *where, int *out) {
    if (!parse_and_condition(p, where, out)) {
        return 0;
    }

    while (match(p, TOKEN_OR)) {
        int right;

        if (!parse_and_condition(p, where, &right) ||
            !join_conditions(p, where, COND_OR, *out, right, out)) {
            return 0;
        }
    }

    return 1;
}

// [ WHERE condition ], the end of SELECT and DELETE. Sets *has_where to
// whether there was one.
static int parse_where(Parser *p, int *has_where, WhereClause *where) {
    *has_where = match(p, TOKEN_WHERE);
    where->condition_count = 0;

    if (!*has_where) {
        return 1;
    }

    if (!parse_condition(p, where, &where->root)) {
        return 0;
    }

    // A condition can always be extended, so name those options
    // rather than the bare "expected ';'" from parse.
    if (peek(p)->type != TOKEN_SEMICOLON) {
        error_expected(p, "AND, OR or ';'");
        return 0;
    }

    return 1;
}

// select_stmt = SELECT select_list FROM IDENTIFIER [ WHERE condition ]
static int parse_select(Parser *p, Statement *out) {
    SelectStatement *select = &out->as.select;

    if (!expect(p, TOKEN_SELECT, "SELECT") ||
        !parse_select_list(p, select)) {
        return 0;
    }

    // After a column name, a comma would also have been valid.
    if (!expect(p, TOKEN_FROM, select->select_all ? "FROM" : "',' or FROM")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    if (!parse_where(p, &select->has_where, &select->where)) {
        return 0;
    }

    out->type = STMT_SELECT;
    snprintf(out->table_name, sizeof(out->table_name), "%s", name->text);
    return 1;
}

// delete_stmt = DELETE FROM IDENTIFIER [ WHERE condition ]
static int parse_delete(Parser *p, Statement *out) {
    DeleteStatement *delete = &out->as.delete;

    if (!expect(p, TOKEN_DELETE, "DELETE") ||
        !expect(p, TOKEN_FROM, "FROM")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    // Without a WHERE, the next token must be ';'.
    if (!parse_where(p, &delete->has_where, &delete->where)) {
        return 0;
    }

    out->type = STMT_DELETE;
    snprintf(out->table_name, sizeof(out->table_name), "%s", name->text);
    return 1;
}

// assignment = IDENTIFIER EQUAL value
static int parse_assignment(Parser *p, Assignment *out) {
    const Token *column = expect(p, TOKEN_IDENTIFIER, "column name");
    if (!column) {
        return 0;
    }

    if (!expect(p, TOKEN_EQUAL, "'='") ||
        !parse_value(p, &out->value)) {
        return 0;
    }

    snprintf(out->column, sizeof(out->column), "%s", column->text);
    return 1;
}

// update_stmt = UPDATE IDENTIFIER
//               SET assignment { COMMA assignment } [ WHERE condition ]
static int parse_update(Parser *p, Statement *out) {
    UpdateStatement *update = &out->as.update;

    if (!expect(p, TOKEN_UPDATE, "UPDATE")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    if (!expect(p, TOKEN_SET, "SET")) {
        return 0;
    }

    update->assignment_count = 0;

    do {
        if (update->assignment_count == MAX_COLUMNS) {
            snprintf(p->error, sizeof(p->error),
                     "too many assignments (max %d)", MAX_COLUMNS);
            return 0;
        }

        if (!parse_assignment(
                p, &update->assignments[update->assignment_count])) {
            return 0;
        }

        update->assignment_count++;
    } while (match(p, TOKEN_COMMA));

    // Name every option here, rather than the bare "expected ';'" from
    // parse.
    TokenType next = peek(p)->type;
    if (next != TOKEN_WHERE && next != TOKEN_SEMICOLON) {
        error_expected(p, "',', WHERE or ';'");
        return 0;
    }

    if (!parse_where(p, &update->has_where, &update->where)) {
        return 0;
    }

    out->type = STMT_UPDATE;
    snprintf(out->table_name, sizeof(out->table_name), "%s", name->text);
    return 1;
}

// drop_stmt = DROP TABLE IDENTIFIER
static int parse_drop(Parser *p, Statement *out) {
    if (!expect(p, TOKEN_DROP, "DROP") ||
        !expect(p, TOKEN_TABLE, "TABLE")) {
        return 0;
    }

    const Token *name = expect(p, TOKEN_IDENTIFIER, "table name");
    if (!name) {
        return 0;
    }

    out->type = STMT_DROP;
    snprintf(out->table_name, sizeof(out->table_name), "%s", name->text);
    return 1;
}

// statement = ( create_stmt | insert_stmt | select_stmt | delete_stmt
//             | update_stmt | drop_stmt ) SEMICOLON EOF
int parse(const TokenList *tokens, Statement *out,
          char *error, size_t error_size) {
    Parser p = { tokens, 0, "" };
    int ok;

    switch (peek(&p)->type) {
        case TOKEN_CREATE:
            ok = parse_create(&p, out);
            break;

        case TOKEN_INSERT:
            ok = parse_insert(&p, out);
            break;

        case TOKEN_SELECT:
            ok = parse_select(&p, out);
            break;

        case TOKEN_DELETE:
            ok = parse_delete(&p, out);
            break;

        case TOKEN_UPDATE:
            ok = parse_update(&p, out);
            break;

        case TOKEN_DROP:
            ok = parse_drop(&p, out);
            break;

        default:
            error_expected(&p,
                           "CREATE, INSERT, SELECT, UPDATE, DELETE or DROP");
            ok = 0;
            break;
    }

    ok = ok &&
         expect(&p, TOKEN_SEMICOLON, "';'") != NULL &&
         expect(&p, TOKEN_EOF, "end of input") != NULL;

    if (!ok) {
        snprintf(error, error_size, "%s", p.error);
        return 0;
    }

    return 1;
}
