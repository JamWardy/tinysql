#include <stdio.h>
#include <string.h>

#include "executor.h"
#include "parser.h"
#include "persist.h"
#include "table.h"
#include "tokenizer.h"

#define MAX_INPUT 1024
#define MAX_ERROR 256

// Reads and discards the rest of the current input line.
static void skip_line(void) {
    int c;

    while ((c = getchar()) != '\n' && c != EOF) {
    }
}

int main(int argc, char **argv) {
    char input[MAX_INPUT];
    char error[MAX_ERROR];
    static TokenList tokens;  // Large, so keep these off the stack.
    static Statement stmt;
    Database db;
    const char *path = NULL;  // NULL: keep the database in memory only.

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [database-file]\n", argv[0]);
        return 1;
    }

    database_init(&db);

    // Stop if the file can't be loaded; carrying on with an empty
    // database would overwrite the file at the first change.
    if (argc == 2) {
        path = argv[1];
        if (!database_load(&db, path, error, sizeof(error))) {
            fprintf(stderr, "Error: cannot load '%s': %s\n", path, error);
            return 1;
        }
    }

    puts("TinySQL — experimental database");
    if (path) {
        printf("Database file: %s\n", path);
    } else {
        puts("In-memory database; pass a file name to keep your data.");
    }
    puts("Type .exit to quit.");

    while (1) {
        printf("sql> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }

        // fgets stops when the buffer is full; without a newline the rest
        // of the line is still waiting and would be read as the next line.
        size_t length = strlen(input);
        if (length == sizeof(input) - 1 && input[length - 1] != '\n') {
            skip_line();
            printf("Error: line too long (max %d characters)\n",
                   MAX_INPUT - 2);
            continue;
        }

        if (strcmp(input, ".exit\n") == 0 ||
            strcmp(input, ".exit") == 0) {
            break;
        }

        if (!tokenize(input, &tokens, error, sizeof(error))) {
            printf("Error: %s\n", error);
            continue;
        }

        // Blank line: nothing to do.
        if (tokens.tokens[0].type == TOKEN_EOF) {
            continue;
        }

        if (!parse(&tokens, &stmt, error, sizeof(error))) {
            printf("Error: %s\n", error);
            continue;
        }

        if (!execute(&db, &stmt, error, sizeof(error))) {
            printf("Error: %s\n", error);
            continue;
        }

        // Rewrite the file after every change, so stopping the program
        // at any point loses nothing that succeeded.
        if (path && stmt.type != STMT_SELECT &&
            !database_save(&db, path, error, sizeof(error))) {
            printf("Error: change not saved: %s\n"
                   "It's kept in memory and will be saved with the next "
                   "change that saves successfully.\n", error);
        }
    }

    database_free(&db);
    return 0;
}
