#ifndef TINYSQL_PERSIST_H
#define TINYSQL_PERSIST_H

#include <stddef.h>

#include "table.h"

// Writes all of db to path. The file is replaced only once the new copy
// is complete, so a failure part-way leaves the previous version intact.
// Returns 1, or writes a message to error and returns 0.
int database_save(const Database *db, const char *path,
                  char *error, size_t error_size);

// Loads path into db, which must be empty. A missing file isn't an error:
// db stays empty and the file is created by the first save. On failure
// (unreadable or invalid file) writes a message to error, leaves db
// empty and returns 0.
int database_load(Database *db, const char *path,
                  char *error, size_t error_size);

#endif
