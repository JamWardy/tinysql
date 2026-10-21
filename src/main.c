#include <stdio.h>
#include <string.h>

#include "executor.h"
#include "parser.h"
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

int main(void) {
    char input[MAX_INPUT];
    char error[MAX_ERROR];
    static TokenList tokens;  // Large, so keep these off the stack.
    static Statement stmt;
    Database db;

    database_init(&db);

    puts("TinySQL — experimental database");
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
        }
    }

    database_free(&db);
    return 0;
}
