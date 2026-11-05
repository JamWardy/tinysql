#include "like.h"

#include <stdlib.h>
#include <string.h>

#include "error.h"

typedef enum {
    PIECE_END,
    PIECE_ANY_RUN,  // `%`
    PIECE_ANY_ONE,  // `_`
    PIECE_LITERAL   // One character, which may have been escaped.
} PieceKind;

// One step of a pattern.
typedef struct {
    PieceKind kind;
    const char *literal;    // PIECE_LITERAL's character, and its bytes.
    size_t literal_length;
    size_t width;           // Bytes of the pattern it takes, escape included.
} Piece;

// Bytes in the UTF-8 character at s, which isn't at the end. A stray or cut
// short sequence counts byte by byte, so bad text still matches something.
static size_t char_length(const char *s) {
    unsigned char lead = (unsigned char)s[0];
    size_t want = lead >= 0xF0 && lead < 0xF8 ? 4
                : lead >= 0xE0 ? 3
                : lead >= 0xC0 ? 2 : 1;

    for (size_t i = 1; i < want; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            return 1;
        }
    }
    return want;
}

static int starts_with_escape(const LikePattern *pattern, const char *p) {
    return pattern->escape_length > 0 &&
           strncmp(p, pattern->escape, pattern->escape_length) == 0;
}

// like_init has checked an escape is never last.
static Piece next_piece(const LikePattern *pattern, const char *p) {
    Piece piece = { PIECE_LITERAL, p, 0, 1 };

    if (*p == '\0') {
        piece.kind = PIECE_END;
        piece.width = 0;
    } else if (starts_with_escape(pattern, p)) {
        piece.literal = p + pattern->escape_length;
        piece.literal_length = char_length(piece.literal);
        piece.width = pattern->escape_length + piece.literal_length;
    } else if (*p == '%') {
        piece.kind = PIECE_ANY_RUN;
    } else if (*p == '_') {
        piece.kind = PIECE_ANY_ONE;
    } else {
        piece.literal_length = char_length(p);
        piece.width = piece.literal_length;
    }
    return piece;
}

static unsigned char fold(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

// Whether piece, not the end or a `%`, matches the character at t.
static int piece_matches(const LikePattern *pattern, const Piece *piece,
                         const char *t) {
    if (piece->kind == PIECE_ANY_ONE) {
        return 1;
    }
    if (char_length(t) != piece->literal_length) {
        return 0;
    }
    for (size_t i = 0; i < piece->literal_length; i++) {
        unsigned char a = (unsigned char)piece->literal[i];
        unsigned char b = (unsigned char)t[i];

        if (pattern->ignore_case ? fold(a) != fold(b) : a != b) {
            return 0;
        }
    }
    return 1;
}

int like_init(LikePattern *out, const char *pattern, const char *escape,
              int ignore_case, Error *error) {
    if (!escape) {
        escape = "\\";
    }

    size_t escape_length = strlen(escape);

    if (escape_length > 0 && char_length(escape) != escape_length) {
        return fail(error, ERROR_INVALID_ESCAPE,
                    "ESCAPE must be one character, but it's '%s'", escape);
    }

    out->text = pattern;
    out->escape = escape;
    out->escape_length = escape_length;
    out->ignore_case = ignore_case;

    for (const char *p = pattern; *p != '\0';) {
        if (starts_with_escape(out, p)) {
            p += escape_length;
            if (*p == '\0') {
                return fail(error, ERROR_INVALID_ESCAPE,
                            "LIKE pattern can't end with its escape "
                            "character, '%s'", escape);
            }
        }
        p += char_length(p);
    }
    return 1;
}

// Matches piece by piece. On a mismatch it goes back to the last `%` and
// lets that take one more character, so each `%` before it stays settled:
// at worst text length × pattern length steps, never exponential.
int like_matches(const LikePattern *pattern, const char *text) {
    const char *p = pattern->text;
    const char *t = text;
    const char *run_p = NULL;  // Just after the last `%`,
    const char *run_t = NULL;  // and where in the text it took up to.

    while (1) {
        Piece piece = next_piece(pattern, p);

        if (piece.kind == PIECE_ANY_RUN) {
            p += piece.width;
            run_p = p;
            run_t = t;
            continue;
        }
        if (*t == '\0') {
            return piece.kind == PIECE_END;
        }
        if (piece.kind != PIECE_END && piece_matches(pattern, &piece, t)) {
            p += piece.width;
            t += char_length(t);
            continue;
        }
        if (!run_p) {
            return 0;
        }
        run_t += char_length(run_t);
        p = run_p;
        t = run_t;
    }
}

// Writes the characters before the first wildcard, unescaped, into out, which
// has room for the pattern's text. Sets *exact if there's no wildcard.
static void like_prefix(const LikePattern *pattern, char *out, size_t size,
                        int *exact) {
    const char *p = pattern->text;
    size_t length = 0;

    *exact = 0;
    while (1) {
        Piece piece = next_piece(pattern, p);

        if (piece.kind == PIECE_END) {
            *exact = 1;
            break;
        }
        if (piece.kind != PIECE_LITERAL ||
            length + piece.literal_length >= size) {
            break;
        }
        memcpy(out + length, piece.literal, piece.literal_length);
        length += piece.literal_length;
        p += piece.width;
    }
    out[length] = '\0';
}

int like_range(const LikePattern *pattern, LikeRange *out) {
    size_t size = strlen(pattern->text) + 1;
    char *buffer = malloc(2 * size);
    int exact;

    if (!buffer) {
        return 0;
    }
    out->lower = buffer;
    like_prefix(pattern, out->lower, size, &exact);
    out->upper_inclusive = exact;
    if (exact) {
        out->upper = out->lower;
        return 1;
    }

    // The first text after every one starting with the prefix: its last byte
    // raised, dropping any 0xFF bytes, which can't be.
    size_t length = strlen(out->lower);

    out->upper = buffer + size;
    memcpy(out->upper, out->lower, length + 1);
    while (length > 0 && (unsigned char)out->upper[length - 1] == 0xFF) {
        length--;
    }
    if (length == 0) {
        out->upper = NULL;
        return 1;
    }
    out->upper[length - 1]++;
    out->upper[length] = '\0';
    return 1;
}

void like_range_free(LikeRange *range) {
    free(range->lower);
}
