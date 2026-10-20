#include <stdio.h>
#include <string.h>

#include "parser.h"
#include "tokenizer.h"

#define MAX_INPUT 1024
#define MAX_ERROR 256

static const char *data_type_name(DataType type) {
    switch (type) {
        case TYPE_INT:  return "INT";
        case TYPE_TEXT: return "TEXT";
    }
    return "UNKNOWN";
}

static void print_statement(const Statement *stmt) {
    switch (stmt->type) {
        case STMT_CREATE: {
            const CreateStatement *create = &stmt->as.create;

            printf("CREATE TABLE %s\n", stmt->table_name);
            for (int i = 0; i < create->column_count; i++) {
                printf("  %-12s %s\n", create->columns[i].name,
                       data_type_name(create->columns[i].type));
            }
            break;
        }

        case STMT_INSERT: {
            const InsertStatement *insert = &stmt->as.insert;

            printf("INSERT INTO %s\n", stmt->table_name);
            for (int i = 0; i < insert->value_count; i++) {
                const Literal *value = &insert->values[i];

                switch (value->type) {
                    case TYPE_INT:
                        printf("  %-6s %lld\n", "INT",
                               literal_as_int(value));
                        break;
                    case TYPE_TEXT:
                        printf("  %-6s '%s'\n", "TEXT",
                               literal_as_text(value));
                        break;
                }
            }
            break;
        }

        case STMT_SELECT: {
            const SelectStatement *select = &stmt->as.select;

            printf("SELECT FROM %s\n", stmt->table_name);
            if (select->select_all) {
                puts("  *");
            }
            for (int i = 0; i < select->column_count; i++) {
                printf("  %s\n", select->columns[i]);
            }
            break;
        }
    }
}

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

        print_statement(&stmt);
    }

    return 0;
}
