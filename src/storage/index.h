#ifndef TINYSQL_INDEX_H
#define TINYSQL_INDEX_H

#include <stddef.h>
#include <stdint.h>

// A hash index: finds a row's position from its key's hash, without reading
// every row. It knows nothing about keys or rows: the caller hashes keys, and
// says which row matches through IndexMatch.
//
// Open addressing with linear probing: entries live in one array, and one whose
// slot is taken goes in the next free slot. The array is kept at most half
// full, so a free slot is never far away.

typedef struct {
    uint64_t hash;  // Kept so growing and removing needn't rehash keys.
    size_t row;
    int used;       // 0 for a free slot.
} IndexSlot;

typedef struct {
    IndexSlot *slots;
    size_t capacity;  // A power of two, or 0 before the first entry.
    size_t count;
} Index;

// context is whatever was passed to index_find.
typedef int (*IndexMatch)(size_t row, const void *context);

void index_init(Index *index);

void index_free(Index *index);

// So that many index_inserts can't fail.
int index_reserve(Index *index, size_t count);

// There must be room (see index_reserve). Doesn't check for a row already under
// the same key; the caller does.
void index_insert(Index *index, uint64_t hash, size_t row);

int index_find(const Index *index, uint64_t hash, IndexMatch matches,
               const void *context, size_t *row);

// row must be in index under hash.
void index_remove(Index *index, uint64_t hash, size_t row);

// For when the row at removed is deleted and later rows move down one place.
// removed must already be out of the index.
void index_shift_rows(Index *index, size_t removed);

#endif
