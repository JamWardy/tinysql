#ifndef TINYSQL_BTREE_H
#define TINYSQL_BTREE_H

#include <stddef.h>

#include "value.h"

// A B+ tree. Entries, each a key of one or more Values and a row position, live
// only in the leaves, which are linked both ways. Inner nodes hold copies of
// keys that route a search to the right leaf. Entries sort by key as ORDER BY
// does (NULLs last), then by row, so each is unique even when keys repeat.

// Most entries in a leaf, or separators in an inner node; at least 4. Building
// with -DBTREE_ORDER=4 makes splits and merges frequent, for trying them out.
#ifndef BTREE_ORDER
#define BTREE_ORDER 32
#endif

typedef struct BTreeNode BTreeNode;

typedef struct {
    size_t width;     // Values per key.
    BTreeNode *root;  // NULL while the tree is empty.
    size_t count;
} BTree;

// Valid only until the tree next changes.
typedef struct {
    const BTree *tree;
    const BTreeNode *leaf;  // NULL once past either end.
    size_t slot;
} BTreeCursor;

void btree_init(BTree *tree, size_t width);

void btree_free(BTree *tree);

// Copies key. The tree mustn't already hold the same key and row.
int btree_insert(BTree *tree, const Value *key, size_t row);

// Can't fail: if memory runs out for the separator a borrow copies, the leaf
// merges with a neighbour if they fit, or else stays under half full, which is
// still a correct tree.
void btree_remove(BTree *tree, const Value *key, size_t row);

// For when the row at removed is deleted and later rows move down one place.
// removed's entry must already be out of the tree.
void btree_shift_rows(BTree *tree, size_t removed);

// The first entry whose first prefix_width values sort after probe's, or with
// them if inclusive; past the end if none. With prefix_width 0, every entry
// qualifies.
void btree_seek_first(const BTree *tree, const Value *probe,
                      size_t prefix_width, int inclusive,
                      BTreeCursor *cursor);

// The last entry whose first prefix_width values sort before probe's, or with
// them if inclusive; past the start if none.
void btree_seek_last(const BTree *tree, const Value *probe,
                     size_t prefix_width, int inclusive,
                     BTreeCursor *cursor);

int btree_cursor_valid(const BTreeCursor *cursor);

// Owned by the tree.
const Value *btree_cursor_key(const BTreeCursor *cursor);
size_t btree_cursor_row(const BTreeCursor *cursor);

void btree_cursor_next(BTreeCursor *cursor);
void btree_cursor_prev(BTreeCursor *cursor);

#endif
