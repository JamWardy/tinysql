#include "transaction.h"

#include "error.h"
#include "persist.h"
#include "tinysql.h"  // For TINYSQL_MAX_ERROR.

// ROLLBACK reloads the file, so a transaction can't run without one.
static int fail_no_file(char *error, size_t error_size) {
    return fail(error, error_size, "transactions need a database file");
}

// Keeps track of whether the file is up to date.
static int save(Database *db, const char *path,
                char *error, size_t error_size) {
    if (!database_save(db, path, error, error_size)) {
        db->unsaved = 1;
        return 0;
    }

    db->unsaved = 0;
    return 1;
}

int transaction_autocommit(Database *db, const char *path,
                           char *error, size_t error_size) {
    char reason[TINYSQL_MAX_ERROR];

    if (!path || db->in_transaction) {
        return 1;
    }

    if (!save(db, path, reason, sizeof(reason))) {
        return fail(error, error_size,
                    "change kept in memory but not saved: %s", reason);
    }

    return 1;
}

int transaction_begin(Database *db, const char *path,
                      char *error, size_t error_size) {
    char reason[TINYSQL_MAX_ERROR];

    if (db->in_transaction) {
        return 1;
    }
    if (!path) {
        return fail_no_file(error, error_size);
    }

    // ROLLBACK reloads the file, so it must hold every change so far.
    if (db->unsaved && !save(db, path, reason, sizeof(reason))) {
        return fail(error, error_size,
                    "cannot BEGIN: earlier changes aren't saved: %s",
                    reason);
    }

    db->in_transaction = 1;
    return 1;
}

int transaction_commit(Database *db, const char *path,
                       char *error, size_t error_size) {
    char reason[TINYSQL_MAX_ERROR];

    if (!db->in_transaction) {
        return 1;
    }
    if (!path) {
        return fail_no_file(error, error_size);
    }

    if (!save(db, path, reason, sizeof(reason))) {
        return fail(error, error_size, "cannot COMMIT: %s", reason);
    }

    db->in_transaction = 0;
    return 1;
}

int transaction_rollback(Database *db, const char *path,
                         char *error, size_t error_size) {
    char reason[TINYSQL_MAX_ERROR];
    Database before;

    if (!db->in_transaction) {
        return 1;
    }
    if (!path) {
        return fail_no_file(error, error_size);
    }

    // Load into a new Database first, so a failure leaves db as it is.
    database_init(&before);
    if (!database_load(&before, path, reason, sizeof(reason))) {
        return fail(error, error_size, "cannot ROLLBACK: %s", reason);
    }

    database_free(db);
    *db = before;  // database_init left no transaction open.
    return 1;
}
