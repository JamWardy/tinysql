#ifndef TINYSQL_STORAGE_LOCK_H
#define TINYSQL_STORAGE_LOCK_H

// Stops a second process opening a database file while one has it, as each
// would keep its own copy in memory and the last to save would win. The lock
// is on path + ".lock", not on path, since saving renames a new file over path
// and the lock would stay with the old one.

#include "error.h"

typedef struct {
    int fd;
} DatabaseLock;

// Fails with ERROR_DATABASE_IN_USE, naming the process that has it, if
// another process holds the lock. Take it before database_load.
int database_lock(const char *path, DatabaseLock *lock, Error *error);

// The lock is released anyway when the process exits, however it exits.
void database_unlock(const DatabaseLock *lock);

#endif
