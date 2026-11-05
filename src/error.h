#ifndef TINYSQL_ERROR_H
#define TINYSQL_ERROR_H

#include <stddef.h>

// Writes a printf-style message to error, cut short to fit, and returns 0, so
// an error path is one line:
//
//     return fail(error, error_size, "no such table '%s'", name);
//
// The format attribute makes GCC and Clang check the arguments as they do for
// printf.
int fail(char *error, size_t error_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

#endif
