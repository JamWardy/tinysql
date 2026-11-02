#ifndef TINYSQL_TINYSQL_H
#define TINYSQL_TINYSQL_H

// The one header a program using TinySQL as a library includes. A
// session looks like this:
//
//   Database db;
//   Result result;
//   char error[TINYSQL_MAX_ERROR];
//
//   database_init(&db);
//   if (!database_load(&db, "shop.db", error, sizeof(error))) { ... }
//
//   Value params[1] = { { .type = TYPE_INT, .as.integer = 5 } };
//
//   result_init(&result);
//   if (tinysql_run(&db, "shop.db", "SELECT * FROM users WHERE id = ?;",
//                   params, 1, &result, error, sizeof(error))) {
//       ... read result.names, result_value(&result, row, column) ...
//   }
//   result_free(&result);
//
//   database_free(&db);
//
// TinySQL isn't safe to use from several threads at once: statements
// are parsed into one shared buffer.

#include <stddef.h>

#include "persist.h"  // For database_load.
#include "result.h"
#include "table.h"    // For Database, database_init and database_free.

// Big enough for any error message TinySQL writes; a smaller buffer
// cuts the message short.
#define TINYSQL_MAX_ERROR 256

// Runs one SQL statement, such as "SELECT * FROM users;", against db and
// fills result, which must be empty, as result_init and result_free
// leave it. After a change, db is saved to path, unless path is NULL.
// The caller frees result with result_free.
//
// params holds param_count values, one for each `?` in sql, in order;
// it may be NULL when param_count is 0. Each is INT, TEXT (with its
// text '\0'-terminated) or NULL. TinySQL copies any text it keeps, so
// params is only read during the call.
//
// On success returns 1. On failure writes a message to error, leaves
// result empty and returns 0. Usually db is then unchanged, but if only
// saving failed the change stays in memory, and is saved with the next
// change that saves successfully.
int tinysql_run(Database *db, const char *path, const char *sql,
                const Value *params, size_t param_count,
                Result *result, char *error, size_t error_size);

#endif
