#ifndef TINYSQL_LIKE_H
#define TINYSQL_LIKE_H

#include <stddef.h>

// A LIKE or ILIKE pattern, checked and ready to match. `%` matches any run of
// characters and `_` any one, counting a UTF-8 character as one; the escape
// character makes the one after it literal. Its text is borrowed, from the
// statement, a parameter or a row.
typedef struct {
    const char *text;
    const char *escape;    // escape_length 0 for none.
    size_t escape_length;
    int ignore_case;       // ILIKE: ASCII letters match either case.
} LikePattern;

// escape is NULL for the default, `\`, or empty for none. Fails if escape is
// longer than one character, or the pattern ends with it.
int like_init(LikePattern *out, const char *pattern, const char *escape,
              int ignore_case, char *error, size_t error_size);

int like_matches(const LikePattern *pattern, const char *text);

// Text sorts byte by byte, so LIKE 'abc%' matches only text from 'abc' up to,
// not including, 'abd'. Both bounds share one allocation.
typedef struct {
    char *lower;          // The prefix; "" when it starts with a wildcard.
    char *upper;          // NULL when no text sorts after every match.
    int upper_inclusive;  // With no wildcards: upper is lower, matched alone.
} LikeRange;

// Fails only when out of memory. Free out with like_range_free.
int like_range(const LikePattern *pattern, LikeRange *out);
void like_range_free(LikeRange *range);

#endif
