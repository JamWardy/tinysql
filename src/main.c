#include <stdio.h>

#include "commands.h"
#include "input.h"
#include "print.h"
#include "tinysql.h"

// Runs one statement, printing its result or any error.
static void run_statement(Database *db, const char *path, const char *sql) {
    char error[TINYSQL_MAX_ERROR];
    Result result;

    result_init(&result);

    if (!tinysql_run(db, path, sql, &result, error, sizeof(error))) {
        printf("Error: %s\n", error);
        return;
    }

    print_result(&result);
    result_free(&result);
}

int main(int argc, char **argv) {
    char statement[MAX_STATEMENT];
    char error[TINYSQL_MAX_ERROR];
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
