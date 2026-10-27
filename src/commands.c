#include "commands.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "text.h"

// One more than the longest table name (MAX_TOKEN_LENGTH - 1 characters)
// needs, so a word cut short to fit is still too long to match one.
#define MAX_WORD (MAX_TOKEN_LENGTH + 1)

// Copies the next space-separated word of *s into word (size bytes) and
// moves *s past it. Returns 0 if there's no word left. A word that
// doesn't fit is cut short.
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

// .tables: the name of every table, in the order they were created.
static void print_tables(const Database *db) {
    if (db->count == 0) {
        puts("(no tables)");
        return;
    }

    for (size_t i = 0; i < db->count; i++) {
        puts(db->tables[i].name);
    }
}

// Prints the CREATE TABLE statement that would make an empty copy of
// table, e.g. CREATE TABLE users (id INT NOT NULL, name TEXT);
static void print_create(const Table *table) {
    printf("CREATE TABLE %s (", table->name);

    for (size_t i = 0; i < table->column_count; i++) {
        const Column *column = &table->columns[i];

        printf("%s%s %s%s", i > 0 ? ", " : "", column->name,
               data_type_name(column->type),
               column->not_null ? " NOT NULL" : "");
    }

    puts(");");
}

// .schema [table]: how every table, or just the one named, was created.
static void print_schema(const Database *db, const char *name) {
    if (name) {
        const Table *table = database_find_table(db, name);

        if (!table) {
            printf("Error: no such table '%s'\n", name);
            return;
        }
        print_create(table);
        return;
    }

    if (db->count == 0) {
        puts("(no tables)");
        return;
    }

    for (size_t i = 0; i < db->count; i++) {
        print_create(&db->tables[i]);
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
