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
- [x] `COUNT(column)`, counting the non-`NULL` values
- [x] `NOT NULL` columns (file format version 3)
- [x] Primary keys, on one column or several, with a hash index that
      `WHERE id = 5` uses instead of reading every row (file format
      version 4)
- [x] Results as data: `execute` fills a `Result` (column names and
      types, then rows of `Value`s) and the REPL prints it
- [x] A library: `tinysql_run` in `src/tinysql.h` runs one statement
      and saves after a change, built into `libtinysql.a`
- [x] `''` inside a string for a quote, as in `'Men''s Shoes'`
- [x] `--` comments, to the end of the line, outside strings
- [x] `UNIQUE`, on one column or several, with `NULL` keys left
      unchecked (file format version 5)
- [x] Parameters: `?` wherever a value goes and in `LIMIT`, with the
      values passed to `tinysql_run`
- [x] `INSERT` with column names, `INSERT INTO users (email,
      password_hash) VALUES (?, ?)`, leaving the others `NULL`, as
      `DESIGN.md` describes under "Inserting"
- [x] Expressions, as `DESIGN.md` describes under "Expressions",
      such as `SET stock = stock - ?`: one tree per statement for
      `SET`, `VALUES`, `WHERE`, the `SELECT` list (with `AS`) and
      `ORDER BY`, with `BOOL`, `TRUE` and `FALSE`
- [x] Transactions: `BEGIN`, `COMMIT` and `ROLLBACK`, as `DESIGN.md`
      describes under "Transactions"
- [x] `SELECT ... FOR UPDATE`, accepted as `DESIGN.md` describes under
      "FOR UPDATE"
- [x] IDs: `AUTOINCREMENT`, and `INSERT ... RETURNING` to get the new
      ID back, as `DESIGN.md` describes under "IDs" and "RETURNING"
      (file format version 6)
- [x] `CREATE INDEX` and `DROP INDEX`, B+ trees, so `orders (item_id,
      expires_at)` serves `item_id = ? AND expires_at > ?`, as
      `DESIGN.md` describes under "Indexes" and `INDEXES.md` explains
      (file format version 7)
- [x] A planner that picks the cheapest way to find a statement's rows
      by estimated cost, and `EXPLAIN` to show it, as `DESIGN.md`
      describes under "Plans" and "EXPLAIN"
- [x] `ANALYZE`, with statistics the planner estimates from, as
      `DESIGN.md` describes under "ANALYZE" (file format version 8)
- [x] `ORDER BY ... LIMIT` through an index, walked in order and
      stopping at the `LIMIT`, as `DESIGN.md` describes under "Indexes"
- [x] `LIKE` and `ILIKE`, for searching item titles, with an index used
      for a pattern's fixed start, as `DESIGN.md` describes under
      "LIKE" and "Indexes"

Next, roughly in order, towards a toy online shop (see "Goal" below):

- [x] A server that links `libtinysql.a`, with a wire protocol for
      statements, parameters and results, as `../DESIGN.md` describes
      it under "Key decisions". Errors carry a code as well as the
      message, so a service can tell a broken `UNIQUE` constraint
      (its `409`, or an order's retry) from any other failure
- [x] Lock the database file while it's open, so a second process
      can't open it, as `../DESIGN.md` describes under "Key decisions"
- [ ] Replication, for search's read replica, as `../DESIGN.md`
      describes it

Later, nice but not needed for the shop:

- [ ] DELETE in one pass (see below)
- [ ] Foreign keys (`REFERENCES`); see "Keys and constraints" below
- [ ] Threads in the server, so statements from several connections
      run at once. TinySQL isn't thread-safe yet: statements are parsed
      into one shared buffer
- [ ] Authentication and roles in the server, needed before it listens
      beyond 127.0.0.1: a password checked when a connection opens, as
      Postgres's startup message is, and roles that limit what each
      client may do, read-only for search, read-write for orders, and
      replication for the replica, as Postgres's `REPLICATION` role
      attribute does. Until then, any local process can read, change
      or copy everything
- [ ] `SELECT ... FOR UPDATE` with real row locks, as Postgres has, so
      orders for different items don't wait for each other. Needs an
      undo log per transaction, as `ROLLBACK` can't reload the file
      while others are open
- [ ] `DEFAULT` values in `CREATE TABLE` (a new file format version),
      which a column left out of an `INSERT` gets instead of `NULL`.
      Then revisit `INSERT` with column names, such as whether an empty
      list `()` should be allowed
- [ ] `||` to join text. Unlike the other operators it makes new text,
      so evaluating needs memory for it
- [ ] `BOOL` columns (a new file format version)
- [ ] `OFFSET`, for page 2 of a list; search pages by `id` instead
- [ ] `GROUP BY`
- [ ] `JOIN`; until then the services run one query per table
- [ ] Paged storage: the file as fixed-size pages, with
      a buffer pool holding only the pages in use, so a database can be
      bigger than memory. Tables and B-tree indexes become pages
- [ ] A write-ahead log (WAL), after paged storage: `COMMIT` appends
      what changed instead of rewriting the whole file, with a
      checkpoint now and then, and recovery that replays the log after
      a crash
- [ ] Inverted indexes, which map pieces of a column's text to the rows
      that have them, of two kinds:
  - [ ] Trigrams, for `ILIKE '%term%'` and `LIKE '%term%'`, which
        read every row without one: each three-letter piece maps to its
        rows, as Postgres's `pg_trgm` does. A search intersects its
        pieces' rows, then checks each with the pattern
  - [ ] Words, for full-text search: each word maps to its rows, as in
        SQLite's FTS5 or Postgres's `tsvector`. It matches whole words
        rather than substrings, so needs SQL of its own to ask for it,
        as their `MATCH` and `@@` are

Smaller tidy-ups:

- [ ] `compile_commands.json` (git-ignored), so clangd jumps from a
      call to the function body
- [x] Split `executor.c` (1,250 lines) into `src/executor/`, one file
      per statement, as `CONVENTIONS.md` describes
- [x] Split `parser.c` (980 lines) into `src/parser/` the same way

## Goal: a toy online shop

The aim is an online shop storing its data in TinySQL. This folder is
only the database. TinySQL gets a server, a program that links
`libtinysql.a` and runs the statements clients send it over a socket.
The shop's services live in the parent folder and talk to that server,
as an app talks to Postgres, so none of the shop is built here. The
list above is what the database needs for the shop to work.

The shop is designed in `../DESIGN.md`. Its schema and seed data live
in `../db/`, not here; `seed/retail.sql` and `shop.db` are the old
shop's, and get deleted.

What already works: an item by its key, and an order's availability
check, `SELECT SUM(quantity) FROM orders WHERE item_id = ? AND status =
'PENDING' AND expires_at > ?`.

Why each item in the list is needed:

- **Transactions.** Reserving is several statements: lock the item,
  count what's reserved, insert the order. If another order slips in
  between, the item is oversold. Since the database is in memory and
  saved whole, `BEGIN` can stop saving, `COMMIT` save, and `ROLLBACK`
  reload the file.
- **`FOR UPDATE`.** The design locks the item row while reserving and
  confirming, so two orders for the last unit can't both see it free.
- **IDs.** Without them, the server takes `MAX(id) + 1`, and two
  requests at once can get the same ID. The new order's ID also goes
  back to the client.
- **`CREATE INDEX`.** The availability check is `WHERE item_id = ? AND
  expires_at > ?` on every order, which reads them all without one. The
  `>` needs the index in order, which `index.c`'s hash isn't.
- **`LIKE`.** Search finds items whose title contains the term,
  ignoring case: `title ILIKE ?`, with `'%shoe%'` as the value.
- **A server.** The services talk to TinySQL over a socket; see
  `../DESIGN.md` for why.
- **The file lock.** Two processes on one file each save their own
  copy, and the last save wins; see `../DESIGN.md`.
- **Replication.** Search reads a replica, so search load never slows
  ordering. The replica may be a few seconds behind.

## DELETE: remove matching rows in one pass

Now, `execute_delete` calls `table_delete_row` per matching row, and each
call `memmove`s every later row down one place, then lowers every later
position in each key's index (`index_shift_rows`). Deleting k of n rows
moves about k × n rows: O(n²) for `DELETE FROM t;`.

Instead, compact in one pass with two indexes: read every row, free the
ones that match, and move each kept row down to the next free position.
Each row is freed or moved once, so it's O(n). Then rebuild each key's
index once from the kept rows, rather than fixing it row by row.

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

## Keys and constraints: one list per table

`PRIMARY KEY`, `UNIQUE` and `REFERENCES` must work across several
columns as well as one, e.g. an order's idempotency key is unique per
user:

```sql
CREATE TABLE orders (id INT PRIMARY KEY, user_id INT,
                     idempotency_key TEXT,
                     UNIQUE (user_id, idempotency_key));
```

That constraint belongs to the pair, not to either column: `UNIQUE` on
each column alone would mean something stricter. So these constraints
can't be per-column flags like `NOT NULL`. Each table instead keeps a
list of constraints, each naming its columns; a single-column constraint
is a list of one. PostgreSQL's `pg_constraint` works the same way.

### Done: PRIMARY KEY

Built this way: `Constraint` in `table.h` (a kind, a `ColumnList` and an
`Index`), filled by `table_add_constraint` from `CREATE TABLE` or the
loader, and stored in the file from version 4. Both SQL forms fill the
same list. `PRIMARY KEY` sets `NOT NULL` on its columns, so the existing
NULL checks cover it. The index (`index.c`) is rebuilt as rows are
loaded, not stored, and `table.c` keeps it up to date through every
change. `INSERT`, `UPDATE` and the loader reject duplicate keys, and a
`WHERE` that pins down a whole key uses the index.

### Done: UNIQUE

Built the same way, as a second kind (`CONSTRAINT_UNIQUE`, kind 2 in
the file from version 5). A table keeps up to `MAX_KEYS` (8) keys, and
rejects two with the same columns. A key with a `NULL` stays out of its
index (`key_has_null`), so `table_find_key` never finds it and it can't
clash. `DESIGN.md` describes how it behaves, under "Keys".

### Later: REFERENCES

Each constraint also needs its parent table, by name because a
`Table *` moves when tables are created or dropped, and the parent's key
columns, one per column. Column positions never change (there's no
`ALTER TABLE`), so positions are safe there. It has no index of its own,
so `find_key_lookup` must skip it.

```c
    char *ref_table;
    ColumnList ref_columns;
```

In the file, after the constraint's columns: `string ref_table`, then
`u8 position` per column. The loader checks the parent table and
columns exist, the types match and the parent columns are its
`PRIMARY KEY` or `UNIQUE`. Parent tables can come later in the file, so
check references after every table is read.

Enforcement, on top of that:

- On `INSERT` and `UPDATE` of the child, the parent must have the key,
  which the parent's index finds. A key with a `NULL` needs no parent.
- On `DELETE`, `UPDATE` of the key and `DROP TABLE` of the parent, no
  child may still refer to it. Refuse these at first (`RESTRICT`);
  `ON DELETE CASCADE` can come later. Without an index on the child's
  columns, this check reads every child row.
