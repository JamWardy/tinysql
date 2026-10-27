#ifndef TINYSQL_TEXT_H
#define TINYSQL_TEXT_H

#include <stddef.h>

// Copying into fixed-size char arrays. Both functions always end dest
// with '\0' and cut the text short if it doesn't fit in size bytes, so
// they never write past the end. size must be at least 1.

// Copies the string src into dest.
void copy_text(char *dest, size_t size, const char *src);

// Copies the length characters starting at start into dest, e.g. one
// word out of the middle of a line. start needn't end in '\0'.
void copy_slice(char *dest, size_t size, const char *start, size_t length);

#endif
