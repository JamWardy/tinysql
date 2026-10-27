#include "error.h"

#include <stdarg.h>
#include <stdio.h>

int fail(char *error, size_t error_size, const char *format, ...) {
    va_list args;

    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
    return 0;
}
