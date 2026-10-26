# TODO

## NULL support: store nulls as a bitmap per row

Decided: when we add `NULL`, the file format marks nulls with a **null
bitmap at the start of each row**, as PostgreSQL does.

```
row = [null bitmap: 1 bit per column, rounded up to whole bytes]
      [each non-null value, stored as in format version 1]
```

- Bit `i` set means column `i` is null: `bitmap[i / 8] & (1 << (i % 8))`.
- A null value has no bytes after the bitmap; the others are unchanged.
- Overhead: 1 byte per row for up to 8 columns, 4 bytes at the max of 32.

This changes the layout, so `FORMAT_VERSION` goes to 2. The loader
should still accept version 1 files (rows with no bitmap, nothing null).

Rejected alternatives:

- **Flag byte per value**: simplest, but costs a byte per value.
- **Type code per value** (SQLite): most compact, but most complex, and
  repeats type information our columns already fix.

## DELETE: remove matching rows in one pass

Now, `execute_delete` calls `table_delete_row` per matching row, and each
call `memmove`s every later row down one place. Deleting k of n rows
moves about k × n rows: O(n²) for `DELETE FROM t;`.

Instead, compact in one pass with two indexes: read every row, free the
ones that match, and move each kept row down to the next free position.
Each row is freed or moved once, so it's O(n).

The storage layer can't evaluate a WHERE, so the executor passes a
function pointer, with a context pointer for the `Filter`:

```c
// Frees every row that matches, keeping the others in order.
// Returns how many rows were deleted.
size_t table_delete_rows(Table *table,
                         int (*matches)(const Row *row, const void *context),
                         const void *context);
```

`table_delete_row` can then go, unless something else needs it.
