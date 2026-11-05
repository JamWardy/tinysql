#include "commands.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "text.h"

// One more than the longest table name (MAX_TOKEN_LENGTH - 1 characters)
// needs, so a word cut short to fit is still too long to match one.
#define MAX_WORD (MAX_TOKEN_LENGTH + 1)

// Moves *s past the word. One that doesn't fit is cut short; returns 0 if none
// is left.
static int next_word(const char **s, char *word, size_t size) {
    while (isspace((unsigned char)**s)) {
        (*s)++;
    }

    const char *start = *s;

    while (**s != '\0' && !isspace((unsigned char)**s)) {
        (*s)++;
    }

    if (*s == start) {
        return 0;
    }

    copy_slice(word, size, start, (size_t)(*s - start));
    return 1;
}

// .tables, in the order they were created.
static void print_tables(const Database *db) {
    if (db->count == 0) {
        puts("(no tables)");
        return;
    }

    for (size_t i = 0; i < db->count; i++) {
        puts(db->tables[i].name);
    }
}

// As the CREATE TABLE that would make an empty copy. A one-column key goes on
// its column, a longer one after the columns.
static void print_create(const Table *table) {
    const ColumnList *primary_key = NULL;

    for (size_t i = 0; i < table->constraint_count; i++) {
        if (table->constraints[i].kind == CONSTRAINT_PRIMARY_KEY) {
            primary_key = &table->constraints[i].columns;
        }
    }

    size_t id_column;
    int has_id = table_autoincrement_column(table, &id_column);

    printf("CREATE TABLE %s (", table->name);

    for (size_t i = 0; i < table->column_count; i++) {
        const Column *column = &table->columns[i];

        printf("%s%s %s", i > 0 ? ", " : "", column->name,
               data_type_name(column->type));

        // PRIMARY KEY makes its columns NOT NULL, so that isn't repeated.
        if (column->not_null &&
            !(primary_key && column_list_has(primary_key, i))) {
            printf(" NOT NULL");
        }

        for (size_t c = 0; c < table->constraint_count; c++) {
            const Constraint *constraint = &table->constraints[c];

            if (constraint->columns.count == 1 &&
                constraint->columns.positions[0] == i) {
                printf(" %s", constraint_kind_name(constraint->kind));
            }
        }

        if (has_id && i == id_column) {
            printf(" AUTOINCREMENT");
        }
    }

    for (size_t c = 0; c < table->constraint_count; c++) {
        const Constraint *constraint = &table->constraints[c];
        const ColumnList *key = &constraint->columns;

        if (key->count == 1) {
            continue;
        }
        printf(", %s (", constraint_kind_name(constraint->kind));
        for (size_t i = 0; i < key->count; i++) {
            printf("%s%s", i > 0 ? ", " : "",
                   table->columns[key->positions[i]].name);
        }
        putchar(')');
    }

    puts(");");
}

static void print_indexes(const Table *table) {
    for (size_t i = 0; i < table->index_count; i++) {
        const TreeIndex *index = &table->indexes[i];

        printf("CREATE INDEX %s ON %s (", index->name, table->name);
        for (size_t c = 0; c < index->columns.count; c++) {
            printf("%s%s", c > 0 ? ", " : "",
                   table->columns[index->columns.positions[c]].name);
        }
        puts(");");
    }
}

// .schema [table], with each table's indexes.
static void print_schema(const Database *db, const char *name) {
    if (name) {
        const Table *table = database_find_table(db, name);

        if (!table) {
            printf("Error: no such table '%s'\n", name);
            return;
        }
        print_create(table);
        print_indexes(table);
        return;
    }

    if (db->count == 0) {
        puts("(no tables)");
        return;
    }

    for (size_t i = 0; i < db->count; i++) {
        print_create(&db->tables[i]);
        print_indexes(&db->tables[i]);
    }
}

int run_command(const Database *db, const char *command) {
    char name[MAX_WORD];
    char argument[MAX_WORD];
    char extra[MAX_WORD];
    const char *rest = command;

    // read_statement only returns commands that start with '.', so
    // there's always a name.
    next_word(&rest, name, sizeof(name));
    int has_argument = next_word(&rest, argument, sizeof(argument));
    int has_extra = next_word(&rest, extra, sizeof(extra));

    if (strcmp(name, ".exit") == 0) {
        if (!has_argument) {
            return 0;
        }
        puts("Error: .exit takes no arguments");
    } else if (strcmp(name, ".tables") == 0) {
        if (has_argument) {
            puts("Error: .tables takes no arguments");
            return 1;
        }
        print_tables(db);
    } else if (strcmp(name, ".schema") == 0) {
        if (has_extra) {
            puts("Error: .schema takes at most one table name");
            return 1;
        }
        print_schema(db, has_argument ? argument : NULL);
    } else {
        printf("Error: unknown command '%s' "
               "(try .tables, .schema or .exit)\n", name);
    }

    return 1;
}
