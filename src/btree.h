#ifndef TINYSQL_BTREE_H
#define TINYSQL_BTREE_H

#include <stddef.h>

#include "value.h"

// A B+ tree: entries kept in key order, each a key of one or more Values
// and the position of the row it came from. Entries live only in the
// leaves, which are linked both ways so a range can be read in order in
// either direction. Inner nodes hold separators, copies of keys that
// route a search down to the right leaf. Every node but the root is at
// least half full, so the tree stays shallow and balanced.
//
// Entries are ordered by key, compared value by value as ORDER BY sorts
// (NULLs last), then by row, so each is unique even when keys repeat.
// The tree owns copies of its keys. Like index.c, it knows nothing about
// tables: the caller passes in each key.

// Most entries in a leaf, and most separators in an inner node. A build
// with -DBTREE_ORDER=4 makes splits and merges happen often, for trying
// them out.
#ifndef BTREE_ORDER
#define BTREE_ORDER 32
#endif

typedef struct BTreeNode BTreeNode;

typedef struct {
    size_t width;     // Values in each key, at least 1.
    BTreeNode *root;  // NULL while the tree is empty.
    size_t count;     // Entries.
} BTree;

// A place among the tree's entries, for reading them in order. Valid
// only until the tree next changes.
typedef struct {
    const BTree *tree;
    const BTreeNode *leaf;  // NULL once past either end.
    size_t slot;            // The entry's position in leaf.
} BTreeCursor;

// Leaves tree empty, for keys of width values.
void btree_init(BTree *tree, size_t width);

// Frees every node and key, and leaves tree empty, ready to reuse.
void btree_free(BTree *tree);

// Adds an entry with a copy of key, which holds width values, for row.
// The tree mustn't already have an entry with the same key and row.
// Returns 1, or 0 if memory ran out (tree is unchanged).
int btree_insert(BTree *tree, const Value *key, size_t row);

// Removes the entry with key and row, which must be in the tree.
// Returns 1, or 0 if memory ran out (tree is unchanged).
int btree_remove(BTree *tree, const Value *key, size_t row);

// Moves every row position after removed down by one, to follow the
// rows when the row at removed is deleted and the ones after it move
// down. removed's entry must already be out of the tree.
void btree_shift_rows(BTree *tree, size_t removed);

// Sets cursor to the first entry whose first prefix_width values sort
// after probe's, or with them too if inclusive. With prefix_width 0,
// every entry sorts with probe. Past the end if there's none.
void btree_seek_first(const BTree *tree, const Value *probe,
                      size_t prefix_width, int inclusive,
                      BTreeCursor *cursor);

// Sets cursor to the last entry whose first prefix_width values sort
// before probe's, or with them too if inclusive. Past the start if
// there's none.
void btree_seek_last(const BTree *tree, const Value *probe,
                     size_t prefix_width, int inclusive,
                     BTreeCursor *cursor);

// Returns 1 if cursor is at an entry, or 0 if it's past either end.
int btree_cursor_valid(const BTreeCursor *cursor);

// The entry's key, width values owned by the tree, and its row. The
// cursor must be valid.
const Value *btree_cursor_key(const BTreeCursor *cursor);
size_t btree_cursor_row(const BTreeCursor *cursor);

// Moves cursor to the next or previous entry in key order, or past the
// end. The cursor must be valid.
void btree_cursor_next(BTreeCursor *cursor);
void btree_cursor_prev(BTreeCursor *cursor);

#endif
