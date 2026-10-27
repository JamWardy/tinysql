#include "index.h"

#include <assert.h>
#include <stdlib.h>

// The size of a new index's array. Grows by doubling.
#define MIN_CAPACITY 16

void index_init(Index *index) {
    index->slots = NULL;
    index->capacity = 0;
    index->count = 0;
}

void index_free(Index *index) {
    free(index->slots);
    index_init(index);
}

// Where the search for hash starts. capacity is a power of two, so
// masking keeps the low bits: the same as hash % capacity, but cheaper.
static size_t home_slot(const Index *index, uint64_t hash) {
    return (size_t)(hash & (index->capacity - 1));
}

// The slot after slot. The mask wraps the last slot round to 0.
static size_t next_slot(const Index *index, size_t slot) {
    return (slot + 1) & (index->capacity - 1);
}

int index_reserve(Index *index, size_t count) {
    // At most half full: with linear probing, a search then reads two
    // or three slots on average. Fuller, runs of used slots join up and
    // searches get long quickly.
    if (count <= index->capacity / 2) {
        return 1;
    }

    size_t capacity = index->capacity == 0 ? MIN_CAPACITY : index->capacity;
    while (count > capacity / 2) {
        if (capacity > SIZE_MAX / 2 / sizeof(IndexSlot)) {
            return 0;  // The array's size in bytes wouldn't fit in a size_t.
        }
        capacity *= 2;
    }

    IndexSlot *slots = calloc(capacity, sizeof(*slots));
    if (!slots) {
        return 0;
    }

    // Every entry's slot depends on the capacity, so each moves.
    Index grown = { slots, capacity, 0 };
    for (size_t i = 0; i < index->capacity; i++) {
        if (index->slots[i].used) {
            index_insert(&grown, index->slots[i].hash, index->slots[i].row);
        }
    }

    free(index->slots);
    *index = grown;
    return 1;
}

void index_insert(Index *index, uint64_t hash, size_t row) {
    // A bug in the caller, which should have called index_reserve.
    assert(index->count < index->capacity / 2);

    size_t slot = home_slot(index, hash);
    while (index->slots[slot].used) {
        slot = next_slot(index, slot);
    }

    index->slots[slot].hash = hash;
    index->slots[slot].row = row;
    index->slots[slot].used = 1;
    index->count++;
}

int index_find(const Index *index, uint64_t hash, IndexMatch matches,
               const void *context, size_t *row) {
    if (index->count == 0) {
        return 0;
    }

    // Every entry under hash is in the unbroken run of used slots from
    // its home slot, so the first free slot ends the search. There's
    // always one, since the array is at most half full.
    for (size_t slot = home_slot(index, hash); index->slots[slot].used;
         slot = next_slot(index, slot)) {
        const IndexSlot *s = &index->slots[slot];

        // Comparing hashes first skips most calls to matches.
        if (s->hash == hash && matches(s->row, context)) {
            *row = s->row;
            return 1;
        }
    }

    return 0;
}

// Returns 1 if slot is in the run from first to last, inclusive, going
// forward and wrapping round the end of the array.
static int in_run(size_t slot, size_t first, size_t last) {
    if (first <= last) {
        return first <= slot && slot <= last;
    }
    return slot >= first || slot <= last;
}

void index_remove(Index *index, uint64_t hash, size_t row) {
    size_t gap = home_slot(index, hash);

    while (!(index->slots[gap].used && index->slots[gap].row == row)) {
        // A bug in the caller: row isn't in the index under hash.
        assert(index->slots[gap].used);
        gap = next_slot(index, gap);
    }

    // A search walks forward from an entry's home slot and stops at the
    // first free slot. So just freeing this slot would hide any later
    // entry whose search passes through it. Check each entry after the
    // gap until the run ends at a free slot, and move the ones whose
    // search would cross the gap back into it (Knuth's Algorithm R).
    size_t next = next_slot(index, gap);
    while (index->slots[next].used) {
        size_t home = home_slot(index, index->slots[next].hash);

        // Home after the gap: the search starts past the gap, so the
        // entry stays. Home at or before the gap: the search reaches the
        // gap, so the entry moves into it, and its old slot becomes the
        // gap.
        if (!in_run(home, next_slot(index, gap), next)) {
            index->slots[gap] = index->slots[next];
            gap = next;
        }
        next = next_slot(index, next);
    }

    index->slots[gap].used = 0;
    index->count--;
}

void index_shift_rows(Index *index, size_t removed) {
    for (size_t i = 0; i < index->capacity; i++) {
        IndexSlot *slot = &index->slots[i];

        // A bug in the caller: removed should be out of the index.
        assert(!slot->used || slot->row != removed);

        if (slot->used && slot->row > removed) {
            slot->row--;
        }
    }
}
