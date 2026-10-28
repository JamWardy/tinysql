#include <stdio.h>

#include "commands.h"
#include "executor.h"
#include "input.h"
#include "parser.h"
#include "persist.h"
#include "print.h"
#include "result.h"
#include "table.h"
#include "tokeniser.h"

#define MAX_ERROR 256

// Tokenises, parses and executes one statement, printing its result or
// any error, and saves the database to path (if any) after a successful
// change.
static void run_statement(Database *db, const char *path, const char *sql) {
    static TokenList tokens;  // Large, so keep these off the stack.
    static Statement stmt;
    char error[MAX_ERROR];
    Result result;

    result_init(&result);

    if (!tokenise(sql, &tokens, error, sizeof(error)) ||
        !parse(&tokens, &stmt, error, sizeof(error)) ||
        !execute(db, &stmt, &result, error, sizeof(error))) {
        printf("Error: %s\n", error);
        return;
    }

    print_result(&result);
    result_free(&result);

    // Rewrite the file after every change, so stopping the program
    // at any point loses nothing that succeeded.
    if (path && stmt.type != STMT_SELECT &&
        !database_save(db, path, error, sizeof(error))) {
        printf("Error: change not saved: %s\n"
               "It's kept in memory and will be saved with the next "
               "change that saves successfully.\n", error);
    }
}

int main(int argc, char **argv) {
    char statement[MAX_STATEMENT];
    char error[MAX_ERROR];
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
    puts("End each statement with ';'. Type .tables or .schema to see "
         "what's here, .exit to quit.");

    while (1) {
        ReadResult result = read_statement(statement, sizeof(statement));

        if (result == READ_END) {
            break;
        }
        if (result == READ_COMMAND && !run_command(&db, statement)) {
            break;  // .exit
        }
        if (result == READ_STATEMENT) {
            run_statement(&db, path, statement);
        }
    }

    database_free(&db);
    return 0;
}
