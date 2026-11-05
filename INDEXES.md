# Hash indexes and B-trees

TinySQL has both kinds of index. Each `PRIMARY KEY` and `UNIQUE` key has
a hash index (`src/storage/index.c`), and `CREATE INDEX` makes a B+ tree
(`src/storage/btree.c`), the kind SQLite, PostgreSQL and MySQL use by default.
This note says how the two differ, why each is used where it is, and how
the B+ tree works.

## Hash index

Each key is turned into a number, its hash, and the hash picks a slot in
an array. Finding a key means checking that slot, and perhaps the next
few if it's taken (see `index.h`).

- **Good at exact matches.** `WHERE id = 5` takes about one step, with
  10 rows or 10 million.
- **No use for order.** Hashing scatters keys on purpose, so 5, 6 and 7
  land in unrelated slots. `WHERE price < 2000`, `ORDER BY price`,
  `LIKE 'Lap%'` and "the 20 cheapest" all still read every row.

## B-tree

A B-tree keeps keys sorted, in a tree of nodes. Each node holds many
keys, often hundreds, with a child node for each gap between them. A
search starts at the root and picks the right child at each level.

- **Exact matches take about log(n) steps.** That's a little slower than
  a hash, but the tree is shallow because each node holds so many keys:
  a million rows is about 3 levels.
- **Ranges and order are fast.** Find where a range starts, then read on
  in order. `BETWEEN`, `<`, `ORDER BY ... LIMIT` and `LIKE 'abc%'` (the
  names starting "abc" sit together) all use it.
- **Built for disks.** One node fills one disk page, so each level costs
  one read. That's why databases that keep data on disk use them;
  SQLite stores whole tables as B-trees, not only indexes.

## Side by side

| | Hash index | B-tree |
|---|---|---|
| `WHERE id = 5` | About 1 step | About log(n) steps |
| `WHERE price < 2000` | Reads every row | Fast |
| `ORDER BY` and `LIMIT` | Must sort | Already in order |
| `LIKE 'abc%'` | Reads every row | Fast |
| Writing one | About 150 lines (`index.c`) | About 750 (`btree.c`): nodes split when full and merge when emptied |

## Which TinySQL uses where

- **Keys use hash indexes.** A key is looked up by `=` to check it's
  free on every `INSERT` and `UPDATE`, and `WHERE id = ?` finds one row.
  That's what a hash does best, and nothing the shop runs needs a range
  on a key.
- **`CREATE INDEX` makes a B+ tree,** as the shop's availability check,
  `item_id = ? AND expires_at > ?`, needs a range. A hash finds only
  equal values.

Postgres has both too: B-trees by default, and `USING hash` for an index
that only ever answers `=`.

## B-tree or B+ tree

Both are wide, sorted, balanced trees. They differ in where the entries,
each a key and its row, live:

- **A B-tree** keeps entries in every node, inner ones included, each
  key once. Reading a range in order means walking up and down the tree.
- **A B+ tree** keeps entries only in the leaves, which are linked to
  their neighbours. Inner nodes hold separators, copies of keys that
  only route a search. Reading a range means going down once, to where
  it starts, then along the leaves.

Indexes are almost all B+ trees, as their main jobs are ranges and
reading in order, and inner nodes holding only keys fit more of them, so
the tree is shallower. TinySQL's is one.

```
                  [ 40 | 80 ]                  separators
                 /     |     \
[10→r4 20→r9] ⇄ [40→r7 50→r1 60→r5] ⇄ [80→r2 90→r3]   entries, linked
```

## How TinySQL's B+ tree works

- **Entries.** Each holds a copy of its key's values and its row's
  position, ordered by key as `ORDER BY` sorts (`NULL`s last), then by
  position, so each entry is unique even when keys repeat. Copies, not
  row positions, as Postgres's are: on disk, reading a row for every
  comparison would cost a page read each time, so the index keeps its
  own copy.
- **Nodes** hold at most 32 entries, or 32 separators, and every node but
  the root at least 16. `BTREE_ORDER` sets the 32; building with 4 makes
  splits and merges frequent, for trying them out.
- **Inserting.** A full leaf that gains an entry splits in two, and a
  copy of the right half's first key goes up to the parent as a
  separator. A full parent splits too, its middle separator moving up,
  and so on to the root, which splits into a new root. That's the only
  way the tree grows taller, so every leaf stays at the same depth.
- **Removing.** A leaf left under half full borrows an entry from a
  neighbour that can spare one, or else merges with it, taking the
  separator between them out of the parent. A parent left under half
  full does the same, up to the root, which is dropped when it's left
  with one child.
- **Running out of memory** changes nothing. An insert copies its key
  and any separator, and makes any new nodes, before touching the tree.
  A remove only needs memory to copy a separator for a borrow; without
  it, the leaf merges if it fits, or stays under half full, which is
  still a correct tree. So removing can't fail, and `DELETE` and
  `UPDATE` needn't undo anything.
- **Positions move.** Deleting a row moves every later row down one
  place, so every later position in the tree, separators too, goes down
  by one, as in the hash index. That reads every node, as deleting a row
  already moves every later row.
- **Not saved.** The file holds each index's name and columns, and the
  tree is rebuilt as the rows are read, as the keys' hash indexes are.

## Using an index

A `WHERE` uses an index on `(a, b, c)` when it pins `a` with `=`, or `a`
and `b`, and so on, perhaps with a range on the next column. The tree
finds where those rows start, then reads along the leaves until they
end. Each row found is still checked against the whole `WHERE`, so the
index only decides how many rows are read, never which ones match.

Reading along the leaves gives the rows in key order, which is what an
`ORDER BY` on the next column wants: walked forwards for `ASC`, or
backwards along the leaves' back links for `DESC`, with no sort, and
stopping at a `LIMIT`. The leaves are linked both ways for this.
`DESIGN.md` says which index a statement picks, and when.
