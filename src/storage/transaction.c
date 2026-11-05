#include "transaction.h"

#include "error.h"
#include "persist.h"

// ROLLBACK reloads the file, so a transaction can't run without one.
static int fail_no_file(Error *error) {
    return fail(error, ERROR_INVALID_TRANSACTION_STATE,
                "transactions need a database file");
}

// Keeps track of whether the file is up to date.
static int save(Database *db, const char *path, Error *error) {
    if (!database_save(db, path, error)) {
        db->unsaved = 1;
        return 0;
    }

    db->unsaved = 0;
    return 1;
}

int transaction_autocommit(Database *db, const char *path, Error *error) {
    Error reason;

    if (!path || db->in_transaction) {
        return 1;
    }

    if (!save(db, path, &reason)) {
        return fail(error, ERROR_CHANGE_NOT_SAVED,
                    "change kept in memory but not saved: %s",
                    reason.message);
    }

    return 1;
}

int transaction_begin(Database *db, const char *path, Error *error) {
    Error reason;

    if (db->in_transaction) {
        return 1;
    }
    if (!path) {
        return fail_no_file(error);
    }

    // ROLLBACK reloads the file, so it must hold every change so far.
    if (db->unsaved && !save(db, path, &reason)) {
        return fail(error, reason.code,
                    "cannot BEGIN: earlier changes aren't saved: %s",
                    reason.message);
    }

    db->in_transaction = 1;
    return 1;
}

int transaction_commit(Database *db, const char *path, Error *error) {
    Error reason;

    if (!db->in_transaction) {
        return 1;
    }
    if (!path) {
        return fail_no_file(error);
    }

    if (!save(db, path, &reason)) {
        return fail(error, reason.code, "cannot COMMIT: %s",
                    reason.message);
    }

    db->in_transaction = 0;
    return 1;
}

int transaction_rollback(Database *db, const char *path, Error *error) {
    Error reason;
    Database before;

    if (!db->in_transaction) {
        return 1;
    }
    if (!path) {
        return fail_no_file(error);
    }

    // Load into a new Database first, so a failure leaves db as it is.
    database_init(&before);
    if (!database_load(&before, path, &reason)) {
        return fail(error, reason.code, "cannot ROLLBACK: %s",
                    reason.message);
    }

    database_free(db);
    *db = before;  // database_init left no transaction open.
    return 1;
}
