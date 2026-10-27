#ifndef TINYSQL_ERROR_H
#define TINYSQL_ERROR_H

#include <stddef.h>

// Writes a printf-style message to error (error_size bytes, cut short if
// it doesn't fit) and returns 0, so an error path is one line:
//
//   return fail(error, error_size, "no such table '%s'", name);
//
// The format attribute is a GCC and Clang extension that makes the
// compiler check the arguments against format, as it does for printf.
int fail(char *error, size_t error_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

#endif
