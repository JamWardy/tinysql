#ifndef TINYSQL_INDEX_H
#define TINYSQL_INDEX_H

#include <stddef.h>
#include <stdint.h>

// A hash index: finds a row's position from the hash of its key, without
// reading every row. It knows nothing about keys or rows themselves; the
// caller hashes keys and says which row matches through IndexMatch, so
// the same index works over a table's rows or any other list.
//
// It's open addressing with linear probing: entries live in one array,
// and an entry whose slot is taken goes in the next free one after it.
// The array is kept at most half full, so a free slot is never far away.

typedef struct {
    uint64_t hash;  // Kept so growing and removing needn't rehash keys.
    size_t row;
    int used;       // 0 for a free slot, whose hash and row are unused.
} IndexSlot;

typedef struct {
    IndexSlot *slots;
    size_t capacity;  // A power of two, or 0 before the first entry.
    size_t count;
} Index;

// Returns 1 if the row at position row has the key being looked for.
// context is whatever the caller passed to index_find.
typedef int (*IndexMatch)(size_t row, const void *context);

void index_init(Index *index);

// Frees the slots and leaves index empty, ready to reuse.
void index_free(Index *index);

// Makes room for count entries in all, so that many index_inserts can't
// fail. Returns 1, or 0 if memory ran out (index is unchanged).
int index_reserve(Index *index, size_t count);

// Adds row under hash. There must be room (see index_reserve). Doesn't
// check for a row already under the same key; the caller does that.
void index_insert(Index *index, uint64_t hash, size_t row);

// Looks among the rows under hash for one that matches. If there is
// one, sets *row to its position and returns 1; otherwise returns 0.
int index_find(const Index *index, uint64_t hash, IndexMatch matches,
               const void *context, size_t *row);

// Removes row, which must be in index under hash.
void index_remove(Index *index, uint64_t hash, size_t row);

// Moves every position after removed down by one, to follow the rows
// when the row at removed is deleted and the ones after it move down.
// removed must already be out of the index.
void index_shift_rows(Index *index, size_t removed);

#endif
