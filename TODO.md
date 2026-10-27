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

Next, roughly in order:

- [ ] DELETE in one pass (see below)
- [ ] Primary key, then a hash index so `WHERE id = 5` doesn't scan
      every row (see "Keys and constraints" below)
- [ ] `UNIQUE` and foreign keys (`REFERENCES`)

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

```c
typedef enum {
    CONSTRAINT_PRIMARY_KEY,
    CONSTRAINT_UNIQUE,
    CONSTRAINT_REFERENCES
} ConstraintKind;

typedef struct {
    ConstraintKind kind;
    size_t columns[MAX_COLUMNS];  // Positions in the table, in key order.
    size_t column_count;
    // For REFERENCES only: the parent table, by name because a Table *
    // moves when tables are created or dropped, and the parent's key
    // columns, one per entry in columns. Column positions never change
    // (there's no ALTER TABLE), so indexes are safe there.
    char *ref_table;
    size_t ref_columns[MAX_COLUMNS];
} Constraint;
```

Both SQL forms fill the same list:

```sql
CREATE TABLE users (id INT PRIMARY KEY, email TEXT UNIQUE);   -- column
CREATE TABLE users (id INT, email TEXT, PRIMARY KEY (id));    -- table
```

In the file (a new format version), after a table's columns:

```
u8 constraint_count
per constraint: u8 kind  u8 column_count  per column: u8 index
                if REFERENCES: string ref_table
                               per column: u8 ref_index
```

`NOT NULL` stays a column flag, since it's only ever about one column.
`PRIMARY KEY` implies `NOT NULL` on each of its columns; set the flags
when the table is created, so the existing NULL checks cover it.

The loader checks, as for everything in the file: column indexes are in
range and not repeated within a constraint, at most one `PRIMARY KEY`,
and for `REFERENCES` that the parent table and columns exist, the types
match and the parent columns are its `PRIMARY KEY` or `UNIQUE`. Parent
tables can come later in the file, so check references after every
table is read.

Enforcement is most of the work, not the format:

- `PRIMARY KEY` and `UNIQUE` need a hash index over the key columns,
  hashing them together, or every `INSERT` and `UPDATE` scans the table.
  The index is rebuilt on load, not stored.
- `REFERENCES` is checked on `INSERT` and `UPDATE` of the child, and on
  `DELETE`, `UPDATE` of the key and `DROP TABLE` of the parent. Refuse
  these at first (`RESTRICT`); `ON DELETE CASCADE` can come later.
- A key with a `NULL` in any column isn't checked, as in standard SQL:
  `UNIQUE` allows several such rows, and `REFERENCES` doesn't need a
  matching parent.
