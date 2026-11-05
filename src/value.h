#ifndef TINYSQL_VALUE_H
#define TINYSQL_VALUE_H

#include <stddef.h>

#include "parser.h"  // For DataType.

// A stored value: its column's type, or TYPE_NULL, for which as is
// unused. When type is TYPE_TEXT, as.text is owned by whatever holds
// the Value, such as a Row. Only an expression's value can be
// TYPE_BOOL, as no column is.
typedef struct {
    DataType type;
    union {
        long long integer;
        char *text;
        int boolean;  // 1 for true, 0 for false.
    } as;
} Value;

// Sets *dest to a copy of src, with its own copy of any text.
// Returns 1, or 0 if memory ran out (*dest is unchanged).
int value_copy(Value *dest, const Value *src);

// Frees the text owned by the first count values, which may be NULL or
// any type. The array itself is the caller's.
void values_free(Value *values, size_t count);

// Returns <0, 0 or >0 as a sorts before, with or after b, as ORDER BY
// sorts them. Both are the same type or NULL, and NULL sorts after
// every other value, as in PostgreSQL. Text compares byte by byte, so
// 'B' < 'a' and '10' < '9'.
int value_compare(const Value *a, const Value *b);

#endif
