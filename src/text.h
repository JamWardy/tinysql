#ifndef TINYSQL_TEXT_H
#define TINYSQL_TEXT_H

#include <stddef.h>

// Both always end dest with '\0', cutting the text short to fit size bytes (at
// least 1).

void copy_text(char *dest, size_t size, const char *src);

// start needn't end in '\0'.
void copy_slice(char *dest, size_t size, const char *start, size_t length);

// strdup, which C11 lacks. NULL if memory ran out.
char *copy_string(const char *s);

#endif
