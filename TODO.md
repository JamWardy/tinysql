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

Next, roughly in order, towards a toy online shop (see "Goal" below):

- [ ] Return results as data rather than printing them, so other
      programs can use TinySQL as a library
- [ ] `''` inside a string for a quote, as in `'Men''s Shoes'`
- [ ] `UNIQUE`, for logging in by email; see "Keys and constraints"
      below
- [ ] Parameters: `WHERE email = ?`, with the values passed separately
- [ ] Arithmetic in `SET` and `WHERE`: `SET stock = stock - 2 WHERE
      stock >= 2`, and `SUM(quantity * unit_price_cents)`
- [ ] Transactions: `BEGIN`, `COMMIT` and `ROLLBACK`
- [ ] IDs: `AUTOINCREMENT`, with a way to get the new ID back
- [ ] `CREATE INDEX` on columns that aren't a key, such as
      `orders.user_id`

Later, nice but not needed for the shop:

- [ ] DELETE in one pass (see below)
- [ ] Foreign keys (`REFERENCES`); see "Keys and constraints" below
- [ ] `OFFSET`, for page 2 of a list
- [ ] `LIKE`, for searching product names
- [ ] `GROUP BY`, for totals per category
- [ ] `JOIN`; until then the server runs one query per table

Smaller tidy-ups:

- [ ] GRAMMAR.md: split `primary = LPAREN condition RPAREN | comparison`
      out of `not_condition`, whose name suggests it's always a NOT
- [ ] `compile_commands.json` (git-ignored), so clangd jumps from a
      call to the function body

## Goal: a toy online shop

The aim is a small web server for an online shop, storing its data in
TinySQL. This folder is only the database. The web server, its HTTP API
and everything else about the shop will live in a parent folder that
uses TinySQL as a library, so none of that is built here. The list
above is what the database needs for the shop to work.

`seed/retail.sql` has the tables: users, categories, products,
addresses, orders, order_items, payments and reviews. Prices are INT
pence, and dates are TEXT like `'2026-01-15'`, which sorts in date
order, so neither needs a new type.

What already works: a product page by its key, products in a category
sorted by price with `LIMIT`, and a basket as `cart_items` with
`PRIMARY KEY (user_id, product_id)`.

Why each item in the list is needed:

- **Results as data.** `execute` prints with `printf`, so only the REPL
  can use it. Instead it should fill a result (column names, then rows
  of `Value`s) for the REPL to print, as `select_rows` fills rows for
  `print_rows`. The server in the parent folder can then link
  TinySQL in as a library, as programs do with SQLite.
- **`''` in strings.** The tokeniser ends a string at the first `'`, so
  `Men's Shoes` can't be stored.
- **`UNIQUE`.** Two accounts mustn't share an email, and logging in
  looks one up, which the index makes fast.
- **Parameters.** Building SQL from what a visitor types lets them write
  SQL of their own (SQL injection). With `?`, a value can only ever be a
  value.
- **Arithmetic.** Checkout lowers the stock, but only if there's
  enough: `UPDATE products SET stock = stock - 2 WHERE id = 5 AND
  stock >= 2`. Today `SET` and comparisons only take a literal.
- **Transactions.** Checkout is several statements: the order, its
  items, the stock and the payment. If one fails, none should happen.
  Since the database is in memory and saved whole, `BEGIN` can stop
  saving, `COMMIT` save, and `ROLLBACK` reload the file.
- **IDs.** Without them, the server takes `MAX(id) + 1`, and two
  requests at once can get the same ID. It also needs the new order's
  ID to add its items.
- **`CREATE INDEX`.** "My orders" is `WHERE user_id = ?`, which reads
  every order without one. It reuses `index.c`.

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
columns as well as one, e.g. `order_items` keyed on the pair:

```sql
CREATE TABLE order_items (order_id INT, product_id INT, quantity INT,
                          PRIMARY KEY (order_id, product_id));
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

### Next: UNIQUE

Mostly a new kind, as it works like `PRIMARY KEY` with two differences:

- Its columns may be `NULL`, and a key with a `NULL` in any column isn't
  checked, as in standard SQL, so several such rows are allowed. Those
  rows stay out of the index: `key_hash` asserts there's no `NULL`.
- A table can have several. `find_key_lookup` already tries every
  constraint, so a `WHERE` on any of them would use its index.

### Then: REFERENCES

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
