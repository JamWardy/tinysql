#ifndef TINYSQL_TRANSACTION_H
#define TINYSQL_TRANSACTION_H

// When changes reach the file. Every statement runs in a transaction: one
// opened by BEGIN, or else its own, which commits as soon as the statement
// succeeds (autocommit). Inside BEGIN, changes stay in memory until COMMIT
// saves them, or ROLLBACK reloads the file, which still holds the database as
// it was at BEGIN.

#include <stddef.h>

#include "error.h"
#include "table.h"

// Saves a change made outside BEGIN; does nothing if path is NULL or a
// transaction is open. If saving fails, the change stays in memory and db is
// marked unsaved.
int transaction_autocommit(Database *db, const char *path, Error *error);

// Fails without a file, as ROLLBACK reloads it, or if an earlier change is
// unsaved and still can't be saved, as ROLLBACK would lose it. Does nothing if
// a transaction is already open.
int transaction_begin(Database *db, const char *path, Error *error);

// If saving fails, the transaction stays open. Does nothing if none is.
int transaction_commit(Database *db, const char *path, Error *error);

// If loading fails, db and the transaction are unchanged. Does nothing if none
// is open.
int transaction_rollback(Database *db, const char *path, Error *error);

#endif
