#ifndef TINYSQL_TRANSACTION_H
#define TINYSQL_TRANSACTION_H

// When changes reach the file. Every statement runs in a transaction:
// one opened by BEGIN, or else one of its own that commits as soon as
// the statement succeeds (autocommit). Inside BEGIN, changes stay in
// memory until COMMIT saves them, or ROLLBACK reloads the file, which
// still holds the database as it was at BEGIN.
//
// Each function returns 1, or writes a message to error and returns 0.

#include <stddef.h>

#include "table.h"

// Commits a change made outside BEGIN by saving db to path; does
// nothing if path is NULL or a transaction is open. If saving fails,
// the change stays in memory and db is marked unsaved until a later
// save works.
int transaction_autocommit(Database *db, const char *path,
                           char *error, size_t error_size);

// Opens a transaction; does nothing if one is already open. Fails if
// path is NULL, as ROLLBACK needs the file, or if an earlier change is
// unsaved and saving it fails again, as ROLLBACK would lose it.
int transaction_begin(Database *db, const char *path,
                      char *error, size_t error_size);

// Saves db to path and closes the transaction; does nothing if none is
// open. If saving fails, the transaction stays open.
int transaction_commit(Database *db, const char *path,
                       char *error, size_t error_size);

// Reloads db from path, undoing every change since BEGIN, which closes
// the transaction; does nothing if none is open. If loading fails, db
// is unchanged and the transaction stays open.
int transaction_rollback(Database *db, const char *path,
                         char *error, size_t error_size);

#endif
