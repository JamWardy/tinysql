#include "error.h"

#include <stdarg.h>
#include <stdio.h>

int fail(Error *error, ErrorCode code, const char *format, ...) {
    va_list args;

    error->code = code;
    va_start(args, format);
    vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
    return 0;
}

const char *error_sqlstate(ErrorCode code) {
    switch (code) {
        case ERROR_INTERNAL:                    return "XX000";
        case ERROR_OUT_OF_MEMORY:               return "53200";
        case ERROR_IO:                          return "58030";
        case ERROR_DATA_CORRUPTED:              return "XX001";
        case ERROR_DATABASE_IN_USE:             return "55006";
        case ERROR_SYNTAX:                      return "42601";
        case ERROR_PROGRAM_LIMIT:               return "54000";
        case ERROR_UNDEFINED_TABLE:             return "42P01";
        case ERROR_UNDEFINED_COLUMN:            return "42703";
        case ERROR_UNDEFINED_OBJECT:            return "42704";
        case ERROR_DUPLICATE_TABLE:             return "42P07";
        case ERROR_DUPLICATE_COLUMN:            return "42701";
        case ERROR_INVALID_TABLE_DEFINITION:    return "42P16";
        case ERROR_DATATYPE_MISMATCH:           return "42804";
        case ERROR_GROUPING:                    return "42803";
        case ERROR_FEATURE_NOT_SUPPORTED:       return "0A000";
        case ERROR_UNIQUE_VIOLATION:            return "23505";
        case ERROR_NOT_NULL_VIOLATION:          return "23502";
        case ERROR_NUMERIC_OUT_OF_RANGE:        return "22003";
        case ERROR_DIVISION_BY_ZERO:            return "22012";
        case ERROR_INVALID_LIMIT:               return "2201W";
        case ERROR_INVALID_ESCAPE:              return "22025";
        case ERROR_IDS_EXHAUSTED:               return "2200H";
        case ERROR_INVALID_TRANSACTION_STATE:   return "25000";
        case ERROR_IDLE_IN_TRANSACTION_TIMEOUT: return "25P03";
        case ERROR_PROTOCOL_VIOLATION:          return "08P01";
    }
    return "XX000";
}
