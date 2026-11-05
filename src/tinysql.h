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

// Big enough for any message TinySQL writes.
#define TINYSQL_MAX_ERROR 256

// Runs one statement. result must be empty, as result_init leaves it, and the
// caller frees it with result_free. After a change, db is saved to path, unless
// path is NULL or a transaction is open.
//
// params holds one value per `?`, in order: INT, TEXT ('\0'-terminated), BOOL
// or NULL. It's only read during the call.
//
// On failure result is left empty, and db is usually unchanged. But if only
// saving failed, the change stays in memory, and the next save that works saves
// it too.
int tinysql_run(Database *db, const char *path, const char *sql,
                const Value *params, size_t param_count,
                Result *result, char *error, size_t error_size);

#endif
