# TODO

## Checklist

Done:

- [x] Tokeniser and parser
- [x] In-memory storage and executor: `CREATE`, `INSERT`, `SELECT`
- [x] Persistence: binary file, rewritten via a temp file and rename
- [x] `WHERE` with comparisons, `AND`, `OR`, `NOT` and parentheses
- [x] Negative numbers
- [x] `DELETE`, `UPDATE`, `DROP TABLE`
- [x] `ORDER BY` and `LIMIT`
- [x] Statements across several lines
- [x] `COUNT(*)`, `SUM`, `MIN` and `MAX`
- [x] `.tables` and `.schema` commands
- [x] `NULL`, stored as a row bitmap (file format version 2)

Next, roughly in order:

- [ ] DELETE in one pass (see below)
- [ ] `COUNT(column)`, counting the non-`NULL` values
- [ ] `NOT NULL` on a column in `CREATE TABLE`
- [ ] Primary key, then a hash index so `WHERE id = 5` doesn't scan
      every row

Smaller tidy-ups:

- [ ] GRAMMAR.md: split `primary = LPAREN condition RPAREN | comparison`
      out of `not_condition`, whose name suggests it's always a NOT
- [ ] `compile_commands.json` (git-ignored), so clangd jumps from a
      call to the function body

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
