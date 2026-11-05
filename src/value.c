#include "value.h"

#include <stdlib.h>
#include <string.h>

#include "text.h"

int value_copy(Value *dest, const Value *src) {
    Value copy = *src;

    if (src->type == TYPE_TEXT) {
        copy.as.text = copy_string(src->as.text);
        if (!copy.as.text) {
            return 0;
        }
    }

    *dest = copy;
    return 1;
}

void values_free(Value *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (values[i].type == TYPE_TEXT) {
            free(values[i].as.text);
        }
    }
}

int value_compare(const Value *a, const Value *b) {
    int a_null = a->type == TYPE_NULL;
    int b_null = b->type == TYPE_NULL;

    if (a_null || b_null) {
        return a_null - b_null;
    }

    switch (a->type) {
        case TYPE_INT: {
            long long x = a->as.integer;
            long long y = b->as.integer;
            return (x > y) - (x < y);
        }
        case TYPE_TEXT:
            return strcmp(a->as.text, b->as.text);
        case TYPE_BOOL:
            return a->as.boolean - b->as.boolean;  // false < true.
        case TYPE_NULL:
            break;  // Handled above.
    }
    return 0;
}
