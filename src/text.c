#include "text.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

void copy_text(char *dest, size_t size, const char *src) {
    copy_slice(dest, size, src, strlen(src));
}

void copy_slice(char *dest, size_t size, const char *start, size_t length) {
    assert(size > 0);

    if (length >= size) {
        length = size - 1;  // Leave room for the '\0'.
    }
    memcpy(dest, start, length);
    dest[length] = '\0';
}

// strdup isn't part of C11, so copy strings by hand.
char *copy_string(const char *s) {
    size_t size = strlen(s) + 1;
    char *copy = malloc(size);

    if (copy) {
        memcpy(copy, s, size);
    }
    return copy;
}
