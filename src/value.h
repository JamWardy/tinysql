#ifndef TINYSQL_VALUE_H
#define TINYSQL_VALUE_H

#include <stddef.h>

#include "parser.h"  // For DataType.

// TYPE_NULL leaves as unused. Text is owned by whatever holds the Value, such
// as a Row. Only an expression's value can be TYPE_BOOL; no column is.
typedef struct {
    DataType type;
    union {
        long long integer;
        char *text;
        int boolean;
    } as;
} Value;

// Copies any text too.
int value_copy(Value *dest, const Value *src);

// Frees the values' text, not the array.
void values_free(Value *values, size_t count);

// Sorts as ORDER BY does: NULL after every other value, as in PostgreSQL, and
// text byte by byte, so 'B' < 'a' and '10' < '9'. a and b are the same type, or
// NULL.
int value_compare(const Value *a, const Value *b);

#endif
