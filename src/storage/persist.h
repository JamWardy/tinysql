#ifndef TINYSQL_PERSIST_H
#define TINYSQL_PERSIST_H

#include <stddef.h>

#include "error.h"
#include "table.h"

// Writes a complete copy beside path, then renames it over path, so a failure
// partway leaves the old file intact.
int database_save(const Database *db, const char *path, Error *error);

// db must be empty. A missing file isn't an error: db stays empty, and the
// first save creates the file. On failure db is left empty.
int database_load(Database *db, const char *path, Error *error);

#endif
