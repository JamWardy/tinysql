#include "text.h"

#include <assert.h>
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
