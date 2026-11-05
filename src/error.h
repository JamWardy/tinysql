#ifndef TINYSQL_ERROR_H
#define TINYSQL_ERROR_H

#include <stddef.h>

// Big enough for any message TinySQL writes.
#define TINYSQL_MAX_ERROR 256

// What kind of failure, so a program can act on it without reading the
// message, such as a shop answering 409 for a duplicate key. Each has
// Postgres's SQLSTATE (see error_sqlstate), so the codes mean what they
// mean there.
typedef enum {
    ERROR_INTERNAL,                   // A bug in TinySQL.
    ERROR_OUT_OF_MEMORY,
    ERROR_IO,                         // Reading or writing the file.
    ERROR_CHANGE_NOT_SAVED,           // Made in memory, but saving failed.
    ERROR_DATA_CORRUPTED,             // The file isn't a valid database.
    ERROR_DATABASE_IN_USE,            // Another process has the file.
    ERROR_SYNTAX,
    ERROR_PROGRAM_LIMIT,              // Too many tokens, columns and so on.
    ERROR_UNDEFINED_TABLE,
    ERROR_UNDEFINED_COLUMN,
    ERROR_UNDEFINED_OBJECT,           // Such as an index.
    ERROR_DUPLICATE_TABLE,            // A table or index name in use.
    ERROR_DUPLICATE_COLUMN,
    ERROR_INVALID_TABLE_DEFINITION,
    ERROR_DATATYPE_MISMATCH,
    ERROR_GROUPING,                   // Aggregates used where they can't be.
    ERROR_FEATURE_NOT_SUPPORTED,
    ERROR_UNIQUE_VIOLATION,
    ERROR_NOT_NULL_VIOLATION,
    ERROR_NUMERIC_OUT_OF_RANGE,
    ERROR_DIVISION_BY_ZERO,
    ERROR_INVALID_LIMIT,
    ERROR_INVALID_ESCAPE,
    ERROR_IDS_EXHAUSTED,
    ERROR_INVALID_TRANSACTION_STATE,
    ERROR_READ_ONLY,                  // A change sent to a replica.
    ERROR_IDLE_IN_TRANSACTION_TIMEOUT,
    ERROR_PROTOCOL_VIOLATION,         // Including a wrong number of values.
} ErrorCode;

typedef struct {
    ErrorCode code;
    char message[TINYSQL_MAX_ERROR];
} Error;

// Writes code and a printf-style message to error, cut short to fit, and
// returns 0, so an error path is one line:
//
//     return fail(error, ERROR_UNDEFINED_TABLE, "no such table '%s'", name);
//
// The format attribute makes GCC and Clang check the arguments as they do for
// printf.
int fail(Error *error, ErrorCode code, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

// Postgres's five-character code, such as "23505" for a duplicate key.
const char *error_sqlstate(ErrorCode code);

#endif
