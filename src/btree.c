#include "btree.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// Fewest entries or separators in a node, the root aside. A full node that
// gains one splits into two at least this full.
#define MIN_COUNT (BTREE_ORDER / 2)

// So a leaf left under half full by btree_remove still has an entry.
_Static_assert(BTREE_ORDER >= 4, "BTREE_ORDER must be at least 4");

// Deeper than any tree that fits in memory: each level multiplies the
// entries by at least MIN_COUNT.
#define MAX_DEPTH 64

// An inner node has count separators and count + 1 children: entries under
// children[i] sort before separator i, and those under children[i + 1] with or
// after it. The arrays have room for one more than BTREE_ORDER, held just
// before a split.
struct BTreeNode {
    int leaf;
    size_t count;

    size_t rows[BTREE_ORDER + 1];             // Each entry's or separator's.
    BTreeNode *children[BTREE_ORDER + 2];     // Inner nodes only.
    BTreeNode *prev;                          // Leaves only: the neighbours
    BTreeNode *next;                          // in key order, or NULL.

    Value keys[];  // width values per entry or separator, in order.
};

// The path from the root to a leaf: node[i] is at depth i, and child[i]
// is which of its children the path goes on to.
typedef struct {
    BTreeNode *node[MAX_DEPTH];
    size_t child[MAX_DEPTH];
    size_t depth;  // The leaf is node[depth - 1].
} Path;

// ---- Keys ----

static Value *key_at(const BTree *tree, const BTreeNode *node, size_t i) {
    return (Value *)&node->keys[i * tree->width];
}

static int compare_prefix(const Value *a, const Value *b, size_t width) {
    for (size_t i = 0; i < width; i++) {
        int c = value_compare(&a[i], &b[i]);
        if (c != 0) {
            return c;
        }
    }
    return 0;
}

// By key, then by row.
static int compare_entries(const BTree *tree, const Value *a_key,
                           size_t a_row, const Value *b_key, size_t b_row) {
    int c = compare_prefix(a_key, b_key, tree->width);

    if (c != 0) {
        return c;
    }
    return (a_row > b_row) - (a_row < b_row);
}

// On failure, nothing is left to free.
static int copy_key(const BTree *tree, Value *out, const Value *key) {
    for (size_t i = 0; i < tree->width; i++) {
        if (!value_copy(&out[i], &key[i])) {
            values_free(out, i);
            return 0;
        }
    }
    return 1;
}

// ---- Nodes ----

static BTreeNode *new_node(const BTree *tree, int leaf) {
    size_t keys = (BTREE_ORDER + 1) * tree->width;
    BTreeNode *node = malloc(sizeof(*node) + keys * sizeof(Value));

    if (node) {
        node->leaf = leaf;
        node->count = 0;
        node->prev = NULL;
        node->next = NULL;
    }
    return node;
}

static void free_node(const BTree *tree, BTreeNode *node) {
    values_free(node->keys, node->count * tree->width);

    if (!node->leaf) {
        for (size_t i = 0; i <= node->count; i++) {
            free_node(tree, node->children[i]);
        }
    }
    free(node);
}

// dest may be src. Ownership of any text moves too.
static void move_keys(const BTree *tree, BTreeNode *dest, size_t to,
                      BTreeNode *src, size_t from, size_t count) {
    memmove(key_at(tree, dest, to), key_at(tree, src, from),
            count * tree->width * sizeof(Value));
    memmove(&dest->rows[to], &src->rows[from], count * sizeof(size_t));
}

// The same, for children.
static void move_children(BTreeNode *dest, size_t to, BTreeNode *src,
                          size_t from, size_t count) {
    memmove(&dest->children[to], &src->children[from],
            count * sizeof(BTreeNode *));
}

// node takes over key's text. Later keys move up a place; node must have room.
static void put_key(const BTree *tree, BTreeNode *node, size_t i,
                    const Value *key, size_t row) {
    move_keys(tree, node, i + 1, node, i, node->count - i);
    memcpy(key_at(tree, node, i), key, tree->width * sizeof(Value));
    node->rows[i] = row;
    node->count++;
}

// Doesn't free the key. Later keys move down a place.
static void take_key(const BTree *tree, BTreeNode *node, size_t i) {
    move_keys(tree, node, i, node, i + 1, node->count - i - 1);
    node->count--;
}

// ---- Searching ----

static size_t child_for_entry(const BTree *tree, const BTreeNode *node,
                              const Value *key, size_t row) {
    size_t i = 0;

    while (i < node->count &&
           compare_entries(tree, key_at(tree, node, i), node->rows[i],
                           key, row) <= 0) {
        i++;
    }
    return i;
}

// The tree mustn't be empty.
static void find_leaf(const BTree *tree, const Value *key, size_t row,
                      Path *path) {
    BTreeNode *node = tree->root;

    path->depth = 0;
    while (1) {
        assert(path->depth < MAX_DEPTH);
        path->node[path->depth] = node;

        if (node->leaf) {
            path->depth++;
            return;
        }

        size_t i = child_for_entry(tree, node, key, row);
        path->child[path->depth++] = i;
        node = node->children[i];
    }
}

// The first slot whose entry sorts with or after key and row.
static size_t slot_in_leaf(const BTree *tree, const BTreeNode *leaf,
                           const Value *key, size_t row) {
    size_t i = 0;

    while (i < leaf->count &&
           compare_entries(tree, key_at(tree, leaf, i), leaf->rows[i],
                           key, row) < 0) {
        i++;
    }
    return i;
}

// Whether an entry with this key comes before what btree_seek_first wants.
static int before_seek_start(const Value *key, const Value *probe,
                         size_t prefix_width, int inclusive) {
    int c = compare_prefix(key, probe, prefix_width);
    return inclusive ? c < 0 : c <= 0;
}

// *slot may be one past the end of the last leaf, when every entry is before
// the target. The tree mustn't be empty.
static void find_first(const BTree *tree, const Value *probe,
                       size_t prefix_width, int inclusive,
                       const BTreeNode **leaf, size_t *slot) {
    const BTreeNode *node = tree->root;

    // A separator that's before the target has every entry left of it
    // before the target too, so the search goes right of it.
    while (!node->leaf) {
        size_t i = 0;

        while (i < node->count &&
               before_seek_start(key_at(tree, node, i), probe, prefix_width,
                             inclusive)) {
            i++;
        }
        node = node->children[i];
    }

    size_t i = 0;
    while (i < node->count &&
           before_seek_start(key_at(tree, node, i), probe, prefix_width,
                         inclusive)) {
        i++;
    }

    // Every entry here is before the target, so it starts the next leaf.
    if (i == node->count && node->next) {
        node = node->next;
        i = 0;
    }

    *leaf = node;
    *slot = i;
}

// ---- Inserting ----

// One per full node from the leaf up, as each will split, plus one for a new
// root if the root splits too.
static size_t nodes_needed(const Path *path) {
    size_t needed = 0;

    for (size_t d = path->depth; d > 0; d--) {
        if (path->node[d - 1]->count < BTREE_ORDER) {
            return needed;
        }
        needed++;
    }
    return needed + 1;
}

// right's first key becomes the separator above them, so the caller copies it
// first.
static void split_leaf(const BTree *tree, BTreeNode *leaf, BTreeNode *right) {
    size_t keep = (BTREE_ORDER + 1) / 2;

    move_keys(tree, right, 0, leaf, keep, leaf->count - keep);
    right->count = leaf->count - keep;
    leaf->count = keep;

    right->prev = leaf;
    right->next = leaf->next;
    if (leaf->next) {
        leaf->next->prev = right;
    }
    leaf->next = right;
}

// Leaves the middle separator past node's end, for the caller to move up.
static void split_inner(const BTree *tree, BTreeNode *node, BTreeNode *right) {
    size_t keep = (BTREE_ORDER + 1) / 2;
    size_t moved = node->count - keep - 1;

    move_keys(tree, right, 0, node, keep + 1, moved);
    move_children(right, 0, node, keep + 1, moved + 1);
    right->count = moved;
    node->count = keep;
}

// The key that will start the right half if leaf splits after a new entry goes
// in at slot.
static const Value *split_key(const BTree *tree, const BTreeNode *leaf,
                              size_t slot, const Value *key) {
    size_t middle = (BTREE_ORDER + 1) / 2;

    if (middle == slot) {
        return key;
    }
    return key_at(tree, leaf, middle < slot ? middle : middle - 1);
}

int btree_insert(BTree *tree, const Value *key, size_t row) {
    Value copy[MAX_COLUMNS];
    Value separator[MAX_COLUMNS];  // For the leaf, if it splits.
    BTreeNode *spare[MAX_DEPTH + 1];
    Path path;

    assert(tree->width <= MAX_COLUMNS);

    if (!tree->root) {
        BTreeNode *root = new_node(tree, 1);

        if (!root || !copy_key(tree, copy, key)) {
            free(root);
            return 0;
        }
        put_key(tree, root, 0, copy, row);
        tree->root = root;
        tree->count = 1;
        return 1;
    }

    // Copy and allocate everything first, so running out of memory
    // leaves the tree as it was.
    find_leaf(tree, key, row, &path);

    BTreeNode *leaf = path.node[path.depth - 1];
    size_t slot = slot_in_leaf(tree, leaf, key, row);
    size_t needed = nodes_needed(&path);
    size_t made = 0;
    int splits = leaf->count == BTREE_ORDER;

    if (!copy_key(tree, copy, key)) {
        return 0;
    }
    if (splits &&
        !copy_key(tree, separator, split_key(tree, leaf, slot, key))) {
        values_free(copy, tree->width);
        return 0;
    }
    while (made < needed) {
        // The first is the leaf's right half; the rest are inner nodes.
        spare[made] = new_node(tree, made == 0);
        if (!spare[made]) {
            break;
        }
        made++;
    }
    if (made < needed) {
        while (made > 0) {
            free(spare[--made]);
        }
        values_free(copy, tree->width);
        if (splits) {
            values_free(separator, tree->width);
        }
        return 0;
    }

    put_key(tree, leaf, slot, copy, row);
    tree->count++;
    if (!splits) {
        return 1;
    }

    // Split each overfull node from the leaf up, passing a separator and
    // the new right node up to its parent each time.
    BTreeNode *right = spare[0];
    size_t up_row;
    Value *up_key = separator;
    size_t next_spare = 1;

    split_leaf(tree, leaf, right);
    up_row = right->rows[0];

    for (size_t d = path.depth - 1; d > 0; d--) {
        BTreeNode *parent = path.node[d - 1];
        size_t i = path.child[d - 1];

        put_key(tree, parent, i, up_key, up_row);
        move_children(parent, i + 2, parent, i + 1, parent->count - i - 1);
        parent->children[i + 1] = right;

        if (parent->count <= BTREE_ORDER) {
            assert(next_spare == made);
            return 1;
        }

        // The middle separator moves up, so it's copied out of parent
        // before split_inner leaves it past parent's end.
        BTreeNode *new_right = spare[next_spare++];
        size_t middle = (BTREE_ORDER + 1) / 2;

        split_inner(tree, parent, new_right);
        memcpy(separator, key_at(tree, parent, middle),
               tree->width * sizeof(Value));
        up_key = separator;
        up_row = parent->rows[middle];
        right = new_right;
    }

    // The root split: a new root holds the one separator.
    BTreeNode *root = spare[next_spare++];

    root->leaf = 0;
    put_key(tree, root, 0, up_key, up_row);
    root->children[0] = tree->root;
    root->children[1] = right;
    tree->root = root;

    assert(next_spare == made);
    return 1;
}

// ---- Removing ----

// The separator between them becomes separator, a copy of the moved entry's key
// made beforehand.
static void borrow_leaf_left(const BTree *tree, BTreeNode *parent,
                             size_t i, Value *separator) {
    BTreeNode *leaf = parent->children[i];
    BTreeNode *left = parent->children[i - 1];
    size_t last = left->count - 1;

    put_key(tree, leaf, 0, key_at(tree, left, last), left->rows[last]);
    left->count--;

    values_free(key_at(tree, parent, i - 1), tree->width);
    memcpy(key_at(tree, parent, i - 1), separator,
           tree->width * sizeof(Value));
    parent->rows[i - 1] = leaf->rows[0];
}

// The separator between them becomes separator, a copy of the right one's new
// first key made beforehand.
static void borrow_leaf_right(const BTree *tree, BTreeNode *parent,
                              size_t i, Value *separator) {
    BTreeNode *leaf = parent->children[i];
    BTreeNode *right = parent->children[i + 1];

    put_key(tree, leaf, leaf->count, key_at(tree, right, 0), right->rows[0]);
    take_key(tree, right, 0);

    values_free(key_at(tree, parent, i), tree->width);
    memcpy(key_at(tree, parent, i), separator, tree->width * sizeof(Value));
    parent->rows[i] = right->rows[0];
}

// Moves the separator before child i down into it, and the last child
// and separator of its left neighbour across and up.
static void borrow_inner_left(const BTree *tree, BTreeNode *parent,
                              size_t i) {
    BTreeNode *node = parent->children[i];
    BTreeNode *left = parent->children[i - 1];

    move_children(node, 1, node, 0, node->count + 1);
    put_key(tree, node, 0, key_at(tree, parent, i - 1), parent->rows[i - 1]);
    node->children[0] = left->children[left->count];

    left->count--;
    move_keys(tree, parent, i - 1, left, left->count, 1);
}

// Moves the separator after child i down into it, and the first child
// and separator of its right neighbour across and up.
static void borrow_inner_right(const BTree *tree, BTreeNode *parent,
                               size_t i) {
    BTreeNode *node = parent->children[i];
    BTreeNode *right = parent->children[i + 1];

    put_key(tree, node, node->count, key_at(tree, parent, i),
            parent->rows[i]);
    node->children[node->count] = right->children[0];

    move_keys(tree, parent, i, right, 0, 1);
    take_key(tree, right, 0);
    move_children(right, 0, right, 1, right->count + 1);
}

// Merges child i + 1 into child i. An inner node takes the separator between
// them down with it; a leaf's is freed, as its entries hold the key already.
static void merge_children(const BTree *tree, BTreeNode *parent, size_t i) {
    BTreeNode *left = parent->children[i];
    BTreeNode *right = parent->children[i + 1];

    if (left->leaf) {
        values_free(key_at(tree, parent, i), tree->width);
        move_keys(tree, left, left->count, right, 0, right->count);
        left->count += right->count;

        left->next = right->next;
        if (right->next) {
            right->next->prev = left;
        }
    } else {
        move_keys(tree, left, left->count, parent, i, 1);
        move_keys(tree, left, left->count + 1, right, 0, right->count);
        move_children(left, left->count + 1, right, 0, right->count + 1);
        left->count += right->count + 1;
    }

    take_key(tree, parent, i);
    move_children(parent, i + 1, parent, i + 2, parent->count - i);
    free(right);
}

// What fixing an underfull leaf will do, worked out before anything
// changes, as borrowing needs a copy of a key.
typedef enum {
    FIX_NONE,
    FIX_BORROW_LEFT,
    FIX_BORROW_RIGHT,
    FIX_MERGE
} LeafFix;

static LeafFix plan_leaf_fix(const Path *path) {
    if (path->depth == 1 || path->node[path->depth - 1]->count > MIN_COUNT) {
        return FIX_NONE;
    }

    const BTreeNode *parent = path->node[path->depth - 2];
    size_t i = path->child[path->depth - 2];

    if (i > 0 && parent->children[i - 1]->count > MIN_COUNT) {
        return FIX_BORROW_LEFT;
    }
    if (i < parent->count && parent->children[i + 1]->count > MIN_COUNT) {
        return FIX_BORROW_RIGHT;
    }
    return FIX_MERGE;
}

// Instead of borrowing, when the separator's copy fails: merge if the two fit
// in one node, which an emptied leaf always does, or else leave the leaf under
// half full.
static LeafFix fallback_fix(const Path *path) {
    const BTreeNode *leaf = path->node[path->depth - 1];
    const BTreeNode *parent = path->node[path->depth - 2];
    size_t i = path->child[path->depth - 2];
    const BTreeNode *other = parent->children[i > 0 ? i - 1 : i + 1];

    // leaf still holds the entry being removed.
    if (leaf->count - 1 + other->count <= BTREE_ORDER) {
        return FIX_MERGE;
    }
    return FIX_NONE;
}

// From the bottom up, then shrinks the root if it's left with one child.
static void fix_inner(BTree *tree, const Path *path) {
    for (size_t d = path->depth - 1; d > 1; d--) {
        BTreeNode *node = path->node[d - 1];
        BTreeNode *parent = path->node[d - 2];
        size_t i = path->child[d - 2];

        if (node->count >= MIN_COUNT) {
            break;
        }
        if (i > 0 && parent->children[i - 1]->count > MIN_COUNT) {
            borrow_inner_left(tree, parent, i);
        } else if (i < parent->count &&
                   parent->children[i + 1]->count > MIN_COUNT) {
            borrow_inner_right(tree, parent, i);
        } else {
            merge_children(tree, parent, i > 0 ? i - 1 : i);
        }
    }

    BTreeNode *root = tree->root;
    if (!root->leaf && root->count == 0) {
        tree->root = root->children[0];
        free(root);
    }
}

void btree_remove(BTree *tree, const Value *key, size_t row) {
    Value separator[MAX_COLUMNS];
    Path path;

    assert(tree->root && tree->width <= MAX_COLUMNS);
    find_leaf(tree, key, row, &path);

    BTreeNode *leaf = path.node[path.depth - 1];
    size_t slot = slot_in_leaf(tree, leaf, key, row);

    // A bug in the caller, which should only remove what it added.
    assert(slot < leaf->count &&
           compare_entries(tree, key_at(tree, leaf, slot), leaf->rows[slot],
                           key, row) == 0);

    // Borrowing makes a new separator, a copy of the key that will start
    // the right-hand leaf. Copy it first, and without the memory, do
    // without borrowing.
    LeafFix fix = plan_leaf_fix(&path);
    BTreeNode *parent = path.depth > 1 ? path.node[path.depth - 2] : NULL;
    size_t i = path.depth > 1 ? path.child[path.depth - 2] : 0;

    if (fix == FIX_BORROW_LEFT) {
        BTreeNode *left = parent->children[i - 1];
        if (!copy_key(tree, separator, key_at(tree, left, left->count - 1))) {
            fix = fallback_fix(&path);
        }
    } else if (fix == FIX_BORROW_RIGHT) {
        BTreeNode *right = parent->children[i + 1];
        if (!copy_key(tree, separator, key_at(tree, right, 1))) {
            fix = fallback_fix(&path);
        }
    }

    values_free(key_at(tree, leaf, slot), tree->width);
    take_key(tree, leaf, slot);
    tree->count--;

    switch (fix) {
        case FIX_NONE:
            if (leaf->count == 0) {  // Only the root can empty.
                free(leaf);
                tree->root = NULL;
            }
            break;
        case FIX_BORROW_LEFT:
            borrow_leaf_left(tree, parent, i, separator);
            break;
        case FIX_BORROW_RIGHT:
            borrow_leaf_right(tree, parent, i, separator);
            break;
        case FIX_MERGE:
            merge_children(tree, parent, i > 0 ? i - 1 : i);
            fix_inner(tree, &path);
            break;
    }
}

// ---- Everything else ----

void btree_init(BTree *tree, size_t width) {
    assert(width > 0);
    tree->width = width;
    tree->root = NULL;
    tree->count = 0;
}

void btree_free(BTree *tree) {
    if (tree->root) {
        free_node(tree, tree->root);
    }
    btree_init(tree, tree->width);
}

static void shift_node(BTreeNode *node, size_t removed) {
    for (size_t i = 0; i < node->count; i++) {
        if (node->rows[i] > removed) {
            node->rows[i]--;
        }
    }
    if (!node->leaf) {
        for (size_t i = 0; i <= node->count; i++) {
            shift_node(node->children[i], removed);
        }
    }
}

void btree_shift_rows(BTree *tree, size_t removed) {
    if (tree->root) {
        shift_node(tree->root, removed);
    }
}

void btree_seek_first(const BTree *tree, const Value *probe,
                      size_t prefix_width, int inclusive,
                      BTreeCursor *cursor) {
    const BTreeNode *leaf = NULL;
    size_t slot = 0;

    assert(prefix_width <= tree->width);
    cursor->tree = tree;

    if (tree->root) {
        find_first(tree, probe, prefix_width, inclusive, &leaf, &slot);
        if (slot == leaf->count) {
            leaf = NULL;  // Every entry was before the target.
        }
    }
    cursor->leaf = leaf;
    cursor->slot = slot;
}

void btree_seek_last(const BTree *tree, const Value *probe,
                     size_t prefix_width, int inclusive,
                     BTreeCursor *cursor) {
    const BTreeNode *leaf;
    size_t slot;

    assert(prefix_width <= tree->width);
    cursor->tree = tree;
    cursor->leaf = NULL;
    cursor->slot = 0;

    if (!tree->root) {
        return;
    }

    // The last entry wanted is the one before the first that isn't.
    find_first(tree, probe, prefix_width, !inclusive, &leaf, &slot);
    if (slot > 0) {
        cursor->leaf = leaf;
        cursor->slot = slot - 1;
    } else if (leaf->prev) {
        cursor->leaf = leaf->prev;
        cursor->slot = leaf->prev->count - 1;
    }
}

int btree_cursor_valid(const BTreeCursor *cursor) {
    return cursor->leaf != NULL;
}

const Value *btree_cursor_key(const BTreeCursor *cursor) {
    assert(cursor->leaf);
    return key_at(cursor->tree, cursor->leaf, cursor->slot);
}

size_t btree_cursor_row(const BTreeCursor *cursor) {
    assert(cursor->leaf);
    return cursor->leaf->rows[cursor->slot];
}

void btree_cursor_next(BTreeCursor *cursor) {
    assert(cursor->leaf);

    if (++cursor->slot == cursor->leaf->count) {
        cursor->leaf = cursor->leaf->next;
        cursor->slot = 0;
    }
}

void btree_cursor_prev(BTreeCursor *cursor) {
    assert(cursor->leaf);

    if (cursor->slot > 0) {
        cursor->slot--;
        return;
    }

    cursor->leaf = cursor->leaf->prev;
    cursor->slot = cursor->leaf ? cursor->leaf->count - 1 : 0;
}
