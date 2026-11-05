# How TinySQL behaves

What each statement does once the parser has accepted it. `GRAMMAR.md`
has what it accepts.

## Expressions

An expression works a value out from values, columns and operators,
such as `stock - ?`, `price * quantity` or `age >= 18 AND name != 'Bob'`.
They go in:

- `SET`: `UPDATE items SET stock = stock - ? WHERE id = ?`
- `VALUES`, but without columns, as there's no row yet:
  `INSERT INTO t VALUES (1 + 1, -?)`
- `WHERE`, on either side of a comparison: `WHERE 18 < age`
- the `SELECT` list, each item optionally named with `AS`:
  `SELECT price * quantity AS total FROM order_items`
- aggregates, and around them: `SUM(price * quantity)`, `MAX(a) - MIN(a)`
- `ORDER BY`: `ORDER BY price * quantity DESC`

`LIMIT` takes only a number or `?`.

### Operators

What each operator takes and gives; `GRAMMAR.md` has how tightly each
binds.

| Operators | Takes | Gives |
|---|---|---|
| `OR`, `AND` | `BOOL`s | `BOOL` |
| `NOT` | a `BOOL` | `BOOL` |
| `=`, `!=` or `<>`, `<`, `<=`, `>`, `>=` | two values of the same type | `BOOL` |
| `IS [NOT] NULL` | any one value | `BOOL` |
| `[NOT] LIKE`, `[NOT] ILIKE` | `TEXT`s (see "LIKE") | `BOOL` |
| `+`, `-`, `*`, `/`, `%` | `INT`s | `INT` |
| `-` before a value | an `INT` | `INT` |

### BOOL

Comparisons, `AND`, `OR` and `NOT` give a `BOOL`: `true` or `false`,
written `TRUE` and `FALSE`. A `WHERE` must be a `BOOL` (or `NULL`), so
`WHERE age` is an error. `BOOL`s compare with `false` before `true`.
A column can't be a `BOOL`; only expressions are.

### Integers

`INT`s are 64-bit. Going past that range is an error, `integer out of
range`, and so is dividing by zero, `division by zero`. `/` drops the
fraction, rounding toward zero (`7 / 2` is 3, `-7 / 2` is -3), and `%`
gives what's left, with the sign of the left side (`-7 % 2` is -1).
These errors happen as rows are read, before anything is changed.

### NULL in expressions

An operator given a `NULL` gives `NULL`: `NULL + 1` and `NULL = NULL`
are both `NULL`. The exceptions are `IS [NOT] NULL`, and `AND` and `OR`,
which follow the table under "NULL" below. `NULL` written straight into
a comparison, as in `age = NULL`, is still an error pointing to
`IS NULL`.

### LIKE

`a LIKE p` is true when the text a matches the pattern p, as in
Postgres. In p, `%` matches any run of characters, none included, and
`_` matches any one character, counting a UTF-8 character such as `é` as
one; any other character matches only itself, byte for byte. The
pattern must match the whole text, so `'shoe%'` finds titles starting
with "shoe" and `'%shoe%'` titles containing it.

`\` before a character makes it match only itself: `'50\%'` matches
only "50%". `ESCAPE` picks another character instead, as in `LIKE
'50!%' ESCAPE '!'`, or none with `ESCAPE ''`. It must be one
character, and a pattern can't end with it.

`ILIKE` is the same but ignores case, for ASCII letters only: `'%shoe%'`
finds "Running Shoes". `NOT LIKE` and `NOT ILIKE` are true when it
doesn't match. All three sides must be `TEXT`, and a `NULL` in any of
them gives `NULL`.

LIKE checks each row it's given; whether rows come from an index is
the planner's choice (see "Indexes").

### SET reads the old row

Every expression in a `SET` reads the row as it was before the
`UPDATE`, so `SET a = b, b = a` swaps the two, as in standard SQL.

### Headings

A `SELECT` item's column is headed by its `AS` name, or its column's
name for a plain column, or otherwise by the expression rebuilt from
how it was parsed, with brackets only where they're needed: `price *
quantity`, `(a + b) * 2`.

## Parameters

A `?` stands for a value passed separately from the SQL, as a program
using TinySQL does (see `tinysql_run` in `src/tinysql.h`):

```sql
INSERT INTO users VALUES (?, ?, ?);
SELECT id FROM users WHERE email = ?;
UPDATE users SET age = ? WHERE id = ?;
SELECT * FROM users ORDER BY id LIMIT ?;
```

The first `?` takes the first value, and so on. A `?` can go wherever a
value can, and after `LIMIT`. Its value is `INT`, `TEXT` or `NULL`, and
is checked like a value written in the SQL: it must suit its column, and
a `LIMIT`'s must be an `INT` that isn't negative. There must be exactly
one value per `?`.

Values passed this way are never read as SQL, so text such as
`x'); DROP TABLE users; --` is just stored as text. That's why programs
should pass what a user types as a parameter rather than writing it into
the SQL. Their text isn't limited to 127 characters as a string written
in the SQL is.

`age = NULL` written in the SQL is an error (see "NULL" below), but a
`?` whose value is `NULL` isn't known until it runs. So, as in other SQL
databases, `age = ?` with `NULL` is unknown and matches no row.

The REPL has no way to pass values, so a statement there with a `?`
fails with `statement has 1 parameters but 0 values were given`.

## Comparing values

`INT` values compare as numbers. `TEXT` values compare byte by byte, so
uppercase sorts before lowercase (`'B' < 'a'`) and digits compare as
characters (`'10' < '9'`). `BOOL`s compare with `false` first.

## NULL

`NULL` means "no value": unknown or missing, such as an age nobody gave.
It isn't 0 or `''`, and any column can hold it unless it's declared
`NOT NULL`. Results print it as `NULL`, so it doesn't look the same as
an empty string.

Because a `NULL` has no value, comparing one with anything, even another
`NULL`, gives neither true nor false but *unknown*. `WHERE` keeps only
the rows where the condition is true, so it leaves out unknown ones:

```sql
SELECT * FROM users WHERE age > 30;       -- leaves out NULL ages
SELECT * FROM users WHERE NOT age > 30;   -- leaves them out too
SELECT * FROM users WHERE age IS NULL;    -- finds them
```

In SQL, `age = NULL` is unknown for every row, so it matches nothing,
which is a common mistake. TinySQL rejects it and points to `IS NULL`.

Unknown follows these rules, called three-valued logic:

| `a`     | `b`     | `a AND b` | `a OR b` | `NOT a` |
|---------|---------|-----------|----------|---------|
| true    | unknown | unknown   | true     | false   |
| false   | unknown | false     | unknown  | true    |
| unknown | unknown | unknown   | unknown  | unknown |

`false AND` anything is false, and `true OR` anything is true, so an
unknown there doesn't matter.

`ORDER BY` puts `NULL`s after every other value: last in ascending
order, first with `DESC`.

## Inserting

An `INSERT` gives a value for every column, in the table's order, or
names the columns it gives values for, in any order:

```sql
INSERT INTO users VALUES (1, 'Alice', 25);
INSERT INTO users (name, id) VALUES ('Ann', 5);
```

A column left out is `NULL`, so it can't be `NOT NULL`, unless it's
`AUTOINCREMENT` (see "IDs"). There must be one value per named column,
and each column is named at most once. `()` with no columns is a
syntax error.

### RETURNING

`RETURNING` gives back the row the `INSERT` added, as in Postgres, so
the client learns what TinySQL filled in, such as a new ID. Its list
is written as a `SELECT` list: `*`, or expressions with `AS`, read from
the new row. Aggregates aren't allowed.

```sql
INSERT INTO users (email) VALUES ('a@b.com') RETURNING id;
INSERT INTO items (title, price) VALUES ('Mug', 800) RETURNING *;
INSERT INTO items (title, price) VALUES ('Mug', 800)
RETURNING id, price * 2 AS double;
```

The result is one row, as a `SELECT`'s would be. It's worked out
before the row goes in, so if it fails, say on an overflow, nothing is
inserted. `RETURNING` is only for `INSERT`, and `AS` is needed for a
heading other than the column's name or the expression.

## Ordering and limiting

`SELECT` filters with `WHERE`, then sorts with `ORDER BY`, then keeps
the first `LIMIT` rows. `ORDER BY` takes any expression of the row, such
as `price * qty`, and it doesn't have to be one of the selected items.

Rows with equal sort values come in no particular order, as in SQL and
Postgres: an index walked backwards for `DESC` gives them newest first,
and a sort oldest first. Without `ORDER BY`, rows come out in whatever
order the plan finds them. Add more to the `ORDER BY` when the order
matters.

## Aggregates

`COUNT`, `SUM`, `MIN` and `MAX` read every row that matches the
`WHERE` and give a single result row. `COUNT(*)` counts those rows.
Each of the others works out its expression for every row:
`SUM(price * qty)` adds them up, and fails if the total doesn't fit in
an `INT`. `MIN` and `MAX` take `INT` or `TEXT`, comparing values as
described above. Aggregates can go inside expressions too, as in
`MAX(age) - MIN(age)` or `COUNT(*) + 1`.

`COUNT(*)` counts rows, including ones with `NULL`s. `COUNT(x)` counts
only the rows where `x` isn't `NULL`. `SUM`, `MIN` and `MAX` skip
`NULL`s, and give `NULL` when there's nothing else to use: no matching
rows, or only `NULL`s. `COUNT` gives 0 instead.

There's no `GROUP BY`, so a `SELECT` with an aggregate gives one row,
and reads columns only inside aggregates. So `ORDER BY` changes nothing
and `LIMIT` matters only when it's 0.

## Keys

A key is one or more columns whose values, together, are different in
every row. There are two kinds: `PRIMARY KEY` and `UNIQUE`. Write a key
on its column, or after the columns when it spans several:

```sql
CREATE TABLE users (id INT PRIMARY KEY, email TEXT NOT NULL UNIQUE);
CREATE TABLE order_items (order_id INT, product_id INT, quantity INT,
                          PRIMARY KEY (order_id, product_id));
CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, idempotency_key TEXT,
                     UNIQUE (user_id, idempotency_key));
```

In `order_items`, each pair is unique: `(1, 10)` and `(1, 11)` can both
be there, but not two `(1, 10)` rows. `UNIQUE (a, b)` is only about the
pair; `UNIQUE` on `a` and on `b` separately would be stricter.

The two kinds differ:

| | `PRIMARY KEY` | `UNIQUE` |
|---|---|---|
| How many | At most one per table | Any number |
| `NULL` | Not allowed: its columns are `NOT NULL` | Allowed, and not checked |

A primary key picks out its row, and `NULL` is no value, so it can't
pick out anything. A `UNIQUE` key with a `NULL` in any of its columns
isn't checked, as in standard SQL: `NULL` isn't equal even to another
`NULL`. So two users can both have no `nick` here:

```sql
CREATE TABLE users (id INT PRIMARY KEY, nick TEXT UNIQUE);
INSERT INTO users VALUES (1, NULL);
INSERT INTO users VALUES (2, NULL);    -- fine
```

A table has at most 8 keys, counting its primary key, and no two may
have the same columns, in any order: `UNIQUE (a, b)` and `UNIQUE (b, a)`
are the same key.

An `INSERT` or `UPDATE` that would give two rows the same key fails,
with nothing changed:

```sql
INSERT INTO users VALUES (1, 'Ann');
INSERT INTO users VALUES (1, 'Bob');   -- duplicate primary key ... id = 1
UPDATE users SET id = 5;               -- fails if users has 2+ rows
```

Each key has an index, so finding a row by its key doesn't read every
row. A `WHERE` uses one when it needs `column = value` for every column
of a key, alone or joined with `AND`:

```sql
SELECT * FROM users WHERE id = 5;                     -- uses the index
SELECT * FROM users WHERE id = 5 AND name = 'Ann';    -- uses it too
SELECT * FROM users WHERE email = 'ann@example.com';  -- the UNIQUE one
SELECT * FROM order_items WHERE order_id = 1 AND product_id = 10;
SELECT * FROM order_items WHERE order_id = 1;  -- part of the key: reads all
SELECT * FROM users WHERE id = 5 OR id = 6;    -- OR: reads every row
```

The same goes for `UPDATE` and `DELETE`.

### IDs

`AUTOINCREMENT` on a table's primary key makes TinySQL choose each new
row's ID, as in SQLite. The key must be one `INT` column, so a table
has at most one:

```sql
CREATE TABLE orders (id INT PRIMARY KEY AUTOINCREMENT, item_id INT);
INSERT INTO orders (item_id) VALUES (5) RETURNING id;   -- 1
INSERT INTO orders VALUES (NULL, 6) RETURNING id;       -- 2
INSERT INTO orders VALUES (10, 7);                      -- 10, given
INSERT INTO orders (item_id) VALUES (8) RETURNING id;   -- 11
```

- An `INSERT` that leaves the ID out, or gives `NULL`, gets one more
  than the highest the table has had. The first is 1.
- An `INSERT` or `UPDATE` may give its own ID, as seed data does. It's
  used if no other row has it, and if it's higher than any so far,
  chosen IDs carry on after it.
- A deleted row's ID is never chosen again, as each table keeps the
  highest ID it has had, rather than working out `MAX(id) + 1`. So an
  ID can stand for its row elsewhere, such as an order's at the
  payment provider.
- `ROLLBACK` takes back the IDs its `INSERT`s chose, so they're chosen
  again; only IDs of committed rows are never reused.
- Once the table has had the highest `INT`, an `INSERT` that leaves the
  ID out fails: `table 'orders' has run out of IDs`.

`.schema` shows it as `id INT PRIMARY KEY AUTOINCREMENT`, however the
key was written.

## Indexes

`CREATE INDEX` makes a B-tree index on one or more columns, so a
`WHERE` on them can find its rows without reading every row:

```sql
CREATE INDEX orders_item_expires ON orders (item_id, expires_at);
SELECT SUM(quantity) FROM orders
WHERE item_id = 5 AND status = 'PENDING' AND expires_at > 1700000000;
DROP INDEX orders_item_expires;
```

The index keeps every row's values of its columns in order, first by
`item_id`, then by `expires_at` among equal `item_id`s, with `NULL`s
last, as `ORDER BY` sorts. So it can find:

- rows with given values in its first columns: `item_id = 5`, or
  `item_id = 5 AND expires_at = 100`;
- and then a range in the next column: `item_id = 5 AND expires_at >
  100`, with `<`, `<=`, `>` or `>=`, either way round (`100 < expires_at`
  too), and a bound at each end if wanted;
- or, for that range, a `LIKE` pattern's fixed start: text sorts byte by
  byte, so `title LIKE 'shoe%'` can only match from `'shoe'` up to, not
  including, `'shof'`, as Postgres does with an index in byte order. A
  pattern with no wildcard is used as `=`. A pattern starting with a
  wildcard has no fixed start, `ILIKE` matches either case and `NOT
  LIKE` either side of the range, so those can't use an index. When the
  column also has a `<`, `>` or the like, that bounds the range instead.

These count only when the `WHERE` needs them, through `AND`s: under an
`OR` or `NOT` it can't use them. The index only narrows down which rows
to read; the whole `WHERE` is still checked on each, so `status =
'PENDING'` above is checked on the rows the index finds. An index on
`(item_id, expires_at)` can't help a `WHERE` on `expires_at` alone, as
those rows are spread through it.

An index also gives its rows in order. So when a `SELECT`'s `ORDER BY`
is the column right after the ones the `WHERE` pins with `=`, or the
index's first column, the rows can come straight from walking it,
forwards for `ASC` or backwards for `DESC`, with no sort, and with a
`LIMIT` the walk stops once it has that many:

```sql
SELECT * FROM orders WHERE item_id = 5 ORDER BY expires_at DESC LIMIT 10;
SELECT * FROM items ORDER BY price LIMIT 20;   -- with an index on price
```

`NULL`s come last in `ASC` and first in `DESC`, as a sort puts them. An
index can be used for its order alone, as in the second query, even when
the `WHERE` doesn't use it.

Whether a statement uses an index, and which, is the planner's choice:
see "Plans".

- Index names share one set of names with tables, as in Postgres and
  SQLite: no index can have a table's name, nor another index's.
- A table has at most 16 indexes. Two on the same columns are allowed.
- An index is built from the rows already there, kept up to date by
  every `INSERT`, `UPDATE` and `DELETE`, and dropped with its table.
- `.schema` shows a table's indexes after it.

See `INDEXES.md` for how the B-tree works.

## Plans

Before a `SELECT`, `UPDATE` or `DELETE` reads any row, TinySQL plans how
to find its rows, as Postgres does. It lists the ways that could work:

- reading every row, which always works;
- a key's own index, when the `WHERE` pins every column of a
  `PRIMARY KEY` or `UNIQUE` key with `=`, which finds at most one row;
- each `CREATE INDEX` index the `WHERE` can use (see "Indexes").

For each it estimates how many rows it would read, and gives it a cost:
1 for each row read in turn, and 4 for each reached through an index,
which costs more per row, as Postgres weighs reading in order against
jumping about. The cheapest wins, and the first listed on a tie, so an
index is used only when it reads under about a quarter of the rows. On a
tiny table, reading every row can beat even a key.

With an `ORDER BY`, a `SELECT` also considers each index that gives its
order (see "Indexes"). A plan that gives the order needs no sort, and
with a `LIMIT` stops once that many rows have matched, so its reads and
cost are cut to the `LIMIT`'s share of the rows it expects to match. Any
other plan reads all its rows and then sorts the matches, costing
n × log₂ n comparisons at a quarter of a row read each, for n matches.
So `ORDER BY ... LIMIT 10` can walk an index for 10 rows rather than
read and sort thousands.

The plan only decides how many rows are read, never which match: the
whole `WHERE` is checked on every row found, whichever way it's found.
A bad estimate makes a statement slow, never wrong.

Estimates come from the table's statistics, once `ANALYZE` has gathered
them (see below). Without statistics, they're Postgres's default
guesses, as fractions of the table's rows:

| Condition | Fraction |
|---|---|
| `x = y` | 0.5% |
| `x <> y` | 99.5% |
| `x > y`, `x >= y`, `x < y`, `x <= y` | a third |
| `x IS NULL` | 0.5% |
| `x IS NOT NULL` | 99.5% |
| `x LIKE y`, `x ILIKE y` | 0.5% |
| `x NOT LIKE y`, `x NOT ILIKE y` | 99.5% |
| `a AND b` | fa × fb, as if a and b were unrelated |
| `a OR b` | fa + fb − fa × fb |
| `NOT a` | 1 − fa |
| `TRUE`, `FALSE` | all, none |

A comparison gets its fraction whatever is on each side, a column, a
value or an expression such as `a + b`, as in Postgres.

An estimate of under one row counts as one, unless the table is empty,
and a `WHERE` that pins a whole key matches at most one row.

### ANALYZE

`ANALYZE orders;` reads every row of `orders`, and `ANALYZE;` every row
of every table, and keeps statistics on each column, as Postgres does:

- the fraction of rows where it's `NULL`;
- how many distinct values it has;
- its most common values, up to 10, each with the fraction of rows that
  have it: those that occur more than once and more often than average,
  or every value when there are 10 or fewer;
- a histogram of the rest: 21 of them, evenly spaced through their
  sorted order, so each of the 20 gaps holds about the same number.

From these, a comparison between a column and a value, or a `?`, is
estimated with the real value:

| Condition | Fraction |
|---|---|
| `c = v` | v's own, if it's a common value; else what the common values leave, shared evenly by the other distinct values |
| `c <> v` | all but the `NULL`s and `c = v` |
| `c < v` and the like | the common values that satisfy it, plus the histogram's share of the rest: the buckets wholly below v, and part of the one v is in (by position for `INT`, half for `TEXT`) |
| `c IS NULL` | the `NULL` fraction |
| `c LIKE p`, `c ILIKE p` | the common values p matches, plus the rest's share as the histogram's values sample it; if none of those match, 0.5% of the rest |
| `c NOT LIKE p`, `c NOT ILIKE p` | all but the `NULL`s and `c LIKE p` |
| comparing with `NULL` | none |

Anything else, such as two columns compared, still gets the default
guess, and so does a value that can't be worked out, as with an
overflow; the statement reports that when it runs. Conditions joined by
`AND` are still multiplied as if unrelated, even two on the same column,
such as `a > 5 AND a < 9`.

Statistics are fractions, so they scale with the table as it grows, but
they're only as fresh as the last `ANALYZE`: if most orders become
`PENDING`, the planner keeps estimating from the old mix until it's run
again. They're saved in the file, and `ROLLBACK` undoes an `ANALYZE` like
any change.

### EXPLAIN

`EXPLAIN` before a `SELECT`, `UPDATE` or `DELETE` shows the plan instead
of running it, as rows: each way considered, the rows it would read, its
cost, which was chosen, and the rows the whole `WHERE` is guessed to
match. It changes nothing.

```sql
EXPLAIN SELECT * FROM orders WHERE item_id = 5 AND expires_at > 1700000000;
```
```
plan                                                | reads  | cost   | chosen | matches
----------------------------------------------------+--------+--------+--------+--------
every row                                           | 100000 | 100000 |        |     167
index orders_item_expires (item_id =, expires_at >) |    167 |    667 | yes    |     167
```

An index walked over a `LIKE` pattern's fixed start shows it as `(title
LIKE prefix)`, and reads the share of rows estimated between its ends,
as a range would. A plan's text ends ", then sort" when it needs one, or ", in order" (and
", backwards") when it gives the `ORDER BY`'s order.

Only the table, the `WHERE` and the `ORDER BY` are checked, so a mistake
elsewhere in the statement shows up only when it runs. It needs a value
for each `?`, as running would. Unlike Postgres's `EXPLAIN`, it shows the ways
it didn't choose, too.

## Transactions

`BEGIN` starts a transaction: the statements after it take effect
together at `COMMIT`, or not at all if it ends with `ROLLBACK`.

```sql
BEGIN;
UPDATE items SET stock = stock - 1 WHERE id = 5;
INSERT INTO orders VALUES (12, 5, 1);
COMMIT;      -- both saved, or ROLLBACK; for neither
```

Outside a transaction, each change is saved to the file as soon as it
succeeds. Inside one, changes are seen by the statements that follow
but stay in memory: `COMMIT` saves them, and `ROLLBACK` throws them
away by loading the file again, which still holds the database as it
was at `BEGIN`. Quitting without `COMMIT` saves nothing. Any statement
can be in a transaction, `CREATE TABLE` and `DROP TABLE` included.

- **A transaction needs a database file.** Without one there's nothing
  for `ROLLBACK` to load, so `BEGIN` fails.
- **A statement that fails ends nothing.** It changes nothing, as
  always, and the transaction carries on, as in SQLite. Whether to
  `ROLLBACK` is up to whoever sent it.
- **`BEGIN` in a transaction, or `COMMIT` or `ROLLBACK` outside one,
  does nothing,** and isn't an error.
- **If `COMMIT` can't save**, it fails and the transaction stays open,
  so it can be retried or rolled back. Likewise if `ROLLBACK` can't
  load the file.
- **If a change outside a transaction failed to save**, the next
  `BEGIN` saves it first, and fails if it still can't, as a `ROLLBACK`
  would otherwise lose it.

`BEGIN TRANSACTION` and `END` aren't accepted.

An ID chosen by an `INSERT` that's rolled back is chosen again, as the
table's highest ID rolls back with everything else (see "IDs").

### FOR UPDATE

`FOR UPDATE`, at the end of a `SELECT`, locks the rows it returns until
the transaction ends, so another transaction can't change them in
between:

```sql
BEGIN;
SELECT stock FROM items WHERE id = ? FOR UPDATE;
UPDATE items SET stock = stock - 1 WHERE id = ?;
COMMIT;
```

TinySQL runs one transaction at a time, so every row is already safe
until `COMMIT` or `ROLLBACK`, and `FOR UPDATE` changes nothing yet. It's
accepted so queries written for row locks run unchanged, and so it's
there when real row locks come.

- It's allowed outside `BEGIN`, as in Postgres, where the lock lasts
  only for that statement.
- It can't be used with aggregates, which give no rows to lock.
- It comes last, after `LIMIT`. `FOR SHARE`, `NOWAIT` and `SKIP LOCKED`
  aren't accepted.

## The REPL

Lines starting with `.` are commands for the program, not SQL. They
skip the tokeniser and parser, need no `;`, and are recognised only at
the start of a statement.

| Command          | Does                                               |
|------------------|----------------------------------------------------|
| `.tables`        | lists the tables, in the order they were created   |
| `.schema`        | prints the `CREATE TABLE` statement for each table |
| `.schema users`  | prints it for one table                            |
| `.exit`          | quits                                              |

The REPL skips a line holding only a comment, as it does a blank line,
and a statement ends at a line whose last character outside strings
and comments is `;`.
