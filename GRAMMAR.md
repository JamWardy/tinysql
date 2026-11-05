# TinySQL grammar

The SQL subset the parser accepts. Each rule is written in terms of the
token types produced by the tokeniser in `src/tokeniser.h`.

## Notation

| Symbol      | Meaning                                  |
|-------------|------------------------------------------|
| `UPPERCASE` | a token type, e.g. `SELECT`, `LPAREN`    |
| `lowercase` | another rule defined in this file        |
| `a b`       | `a` followed by `b`                      |
| `a \| b`    | either `a` or `b`                        |
| `{ a }`     | `a` repeated zero or more times          |
| `[ a ]`     | `a` is optional                          |

## Rules

```
statement     = ( create_stmt | insert_stmt | select_stmt | delete_stmt
                | update_stmt | drop_stmt | transaction_stmt | analyze_stmt
                | EXPLAIN ( select_stmt | update_stmt | delete_stmt ) )
                SEMICOLON EOF

create_stmt   = CREATE TABLE IDENTIFIER
                LPAREN table_element { COMMA table_element } RPAREN
              | CREATE INDEX IDENTIFIER ON IDENTIFIER
                LPAREN IDENTIFIER { COMMA IDENTIFIER } RPAREN
table_element = column_def | table_key

column_def    = IDENTIFIER column_type { column_constraint }
column_type   = INT | TEXT
column_constraint = NOT NULL | PRIMARY KEY | UNIQUE | AUTOINCREMENT
table_key     = ( PRIMARY KEY | UNIQUE )
                LPAREN IDENTIFIER { COMMA IDENTIFIER } RPAREN

insert_stmt   = INSERT INTO IDENTIFIER
                [ LPAREN IDENTIFIER { COMMA IDENTIFIER } RPAREN ]
                VALUES LPAREN expr { COMMA expr } RPAREN
                [ RETURNING select_list ]

select_stmt   = SELECT select_list FROM IDENTIFIER [ WHERE expr ]
                [ ORDER BY expr [ ASC | DESC ] ]
                [ LIMIT ( NUMBER | PARAM ) ] [ FOR UPDATE ]
select_list   = STAR | select_item { COMMA select_item }
select_item   = expr [ AS IDENTIFIER ]

delete_stmt   = DELETE FROM IDENTIFIER [ WHERE expr ]

update_stmt   = UPDATE IDENTIFIER
                SET assignment { COMMA assignment } [ WHERE expr ]
assignment    = IDENTIFIER EQUAL expr

drop_stmt     = DROP ( TABLE | INDEX ) IDENTIFIER

transaction_stmt = BEGIN | COMMIT | ROLLBACK

analyze_stmt  = ANALYZE [ IDENTIFIER ]

expr          = and_expr { OR and_expr }
and_expr      = not_expr { AND not_expr }
not_expr      = NOT not_expr | comparison
comparison    = additive [ compare_op additive | IS [ NOT ] NULL ]
compare_op    = EQUAL | NOT_EQUAL | LESS | LESS_EQUAL
              | GREATER | GREATER_EQUAL
additive      = multiplicative { ( PLUS | MINUS ) multiplicative }
multiplicative = unary { ( STAR | SLASH | PERCENT ) unary }
unary         = MINUS unary | primary
primary       = value | IDENTIFIER | aggregate | LPAREN expr RPAREN
aggregate     = COUNT LPAREN ( STAR | expr ) RPAREN
              | ( SUM | MIN | MAX ) LPAREN expr RPAREN
value         = [ MINUS ] NUMBER | STRING | TRUE | FALSE | NULL | PARAM
```

A `MINUS` right before a `NUMBER` is read as part of the number, not
as `unary`, so `-9223372036854775808` fits although its digits alone
don't.

## Examples

Accepted:

```sql
CREATE TABLE users (id INT, name TEXT, age INT);
CREATE TABLE orders (id INT NOT NULL, item TEXT NOT NULL, note TEXT);
CREATE TABLE products (id INT PRIMARY KEY, name TEXT NOT NULL);
CREATE TABLE order_items (order_id INT, product_id INT, quantity INT,
                          PRIMARY KEY (order_id, product_id));
CREATE TABLE accounts (id INT PRIMARY KEY, email TEXT NOT NULL UNIQUE);
CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, idempotency_key TEXT,
                     UNIQUE (user_id, idempotency_key));
INSERT INTO users VALUES (1, 'Alice', 25);
INSERT INTO users VALUES (-3, 'Eve', 30);
INSERT INTO users VALUES (2, NULL, NULL);    -- any column can be NULL
INSERT INTO users VALUES (4, 'O''Brien', 40);  -- '' is one quote
INSERT INTO users (name, id) VALUES ('Ann', 5);  -- age is left NULL
INSERT INTO users VALUES (6, 'Al', 2 * (10 + 3));
SELECT * FROM users;
SELECT name, age FROM users;
SELECT * FROM users WHERE age >= 18;
SELECT name FROM users WHERE name != 'Bob';   -- `<>` also means not equal
SELECT * FROM users WHERE age >= 18 AND NOT (name = 'Bob' OR id = 1);
SELECT * FROM users WHERE age IS NULL;
SELECT name FROM users WHERE age IS NOT NULL ORDER BY age;
SELECT name FROM users ORDER BY age DESC LIMIT 3;
SELECT * FROM users WHERE age > 18 ORDER BY name;  -- ASC is the default
SELECT COUNT(*) FROM users WHERE age >= 18;
SELECT COUNT(*), SUM(age), MIN(name), MAX(age) FROM users;
SELECT COUNT(*), COUNT(age) FROM users;       -- COUNT(age) skips NULLs
DELETE FROM users WHERE age < 18;
DELETE FROM users;                            -- deletes every row
UPDATE users SET age = 26 WHERE name = 'Alice';
UPDATE users SET name = 'Anon', age = 0;      -- updates every row
UPDATE users SET age = NULL WHERE id = 2;
SELECT * FROM users WHERE name = ? AND age > ?;  -- values passed apart
SELECT * FROM users WHERE 18 <= age;          -- either side
UPDATE items SET stock = stock - ? WHERE id = ?;
SELECT name, price * qty AS total FROM items ORDER BY price * qty DESC;
SELECT SUM(price * qty), MAX(price) - MIN(price) FROM items;
SELECT name, age >= 18 FROM users;            -- true or false
DROP TABLE users;
```

Rejected:

```sql
CREATE TABLE users ();             -- needs at least one column
CREATE TABLE users (id INTEGER);   -- type must be INT or TEXT
CREATE TABLE users (id NOT NULL);  -- NOT NULL goes after the type
CREATE TABLE t (a INT PRIMARY KEY, b INT PRIMARY KEY);  -- one key a table
CREATE TABLE t (a INT, PRIMARY KEY a);   -- key columns go in ( )
CREATE TABLE t (PRIMARY KEY (a));        -- needs at least one column
CREATE TABLE t (a INT, UNIQUE a);        -- key columns go in ( )
INSERT INTO users VALUES (1, 2,);  -- trailing comma
INSERT INTO users VALUES (4, 'O'Brien', 40);  -- write the quote as ''
INSERT INTO users () VALUES ();    -- name at least one column
INSERT INTO users (id, name) VALUES (1);  -- one value per column
SELECT * FROM users                -- missing semicolon
SELECT *, name FROM users;         -- '*' can't be mixed with columns
SELECT * FROM users; SELECT 1;     -- one statement at a time
SELECT * FROM users WHERE a < b < c;  -- comparisons don't chain
SELECT * FROM users WHERE age = NULL;          -- use IS NULL (see below)
SELECT * FROM users WHERE age IS 18;           -- IS only tests for NULL
SELECT * FROM users WHERE age > 1 AND;         -- AND needs a right side
SELECT * FROM users WHERE (age > 1;            -- unclosed '('
SELECT * FROM users LIMIT 3 ORDER BY age;      -- ORDER BY comes first
SELECT * FROM users ORDER BY age, name;        -- one sort expression only
SELECT * FROM users LIMIT -1;                  -- LIMIT can't be negative
DELETE users WHERE id = 1;                     -- needs FROM
DELETE * FROM users;                           -- no column list
UPDATE users age = 26;                         -- needs SET
SELECT a, FROM users;                          -- trailing comma
DROP users;                                    -- needs TABLE
DROP TABLE a, b;                               -- one table at a time
```

## Not checked by the grammar

The parser checks only shape. These are the executor's job, because they
depend on what tables exist:

- the table exists (or, for `CREATE`, doesn't already exist)
- every column an expression names exists, and each operator gets the
  types it takes (see "Expressions")
- a `WHERE` is a `BOOL`; `VALUES` reads no columns; aggregates aren't
  in `WHERE`, `SET` or `VALUES`, nor inside another aggregate
- a `SELECT` with an aggregate reads columns only inside aggregates,
  as there's no `GROUP BY`: `SELECT name, COUNT(*)` is an error
- a `SELECT ... FOR UPDATE` has no aggregates, as they give no rows
  to lock
- `RETURNING` names columns of the table, with no aggregates
- an `AUTOINCREMENT` column is an `INT` and the whole `PRIMARY KEY`
- column names are unique within a `CREATE TABLE`
- each `PRIMARY KEY` and `UNIQUE` column is one of the table's columns,
  named once in that key
- no two keys have the same columns, in any order
- `INSERT` has the right number of values, of the right types (`NULL`
  fits any column that isn't `NOT NULL`)
- `INSERT`'s columns exist, each named at most once, and no column
  left out is `NOT NULL`
- `UPDATE` sets columns that exist, each at most once, to values of the
  right types, or to `NULL` if the column isn't `NOT NULL`
- no two rows have the same primary key or unique key, after an
  `INSERT` or `UPDATE`
- there's one value for each `?`, and a `LIMIT ?`'s is a non-negative
  `INT`

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

From loosest to tightest binding:

| Operators | Takes | Gives |
|---|---|---|
| `OR` | `BOOL`s | `BOOL` |
| `AND` | `BOOL`s | `BOOL` |
| `NOT` | a `BOOL` | `BOOL` |
| `=`, `!=` or `<>`, `<`, `<=`, `>`, `>=`, `IS [NOT] NULL` | two values of the same type; `IS` any one | `BOOL` |
| `+`, `-` | `INT`s | `INT` |
| `*`, `/`, `%` | `INT`s | `INT` |
| `-` before a value | an `INT` | `INT` |

So `a = 1 OR b = 2 AND NOT c + 1 = 3` means
`(a = 1) OR ((b = 2) AND (NOT ((c + 1) = 3)))`. Brackets group
differently. Operators of the same level group from the left, so
`10 - 3 - 2` is 5. A comparison can't be chained: `a < b < c` is an
error.

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

### SET reads the old row

Every expression in a `SET` reads the row as it was before the
`UPDATE`, so `SET a = b, b = a` swaps the two, as in standard SQL.

### Keywords

`AS`, `TRUE` and `FALSE` are keywords, so they can't be used as table
or column names.

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

## Strings

A string is written in single quotes. Inside one, two quotes in a row,
`''`, stand for one quote: `'Men''s Shoes'` is `Men's Shoes`, and
`''''` is a single `'`. On its own, `''` is the empty string. A string
holds at most 127 characters, counting each `''` as one.

## Comments

`--` starts a comment, which runs to the end of the line, except inside
a string: `'a -- b'` is just a string. Comments can go anywhere a space
can, including after the `;`, so `DROP TABLE users; -- old` is one
statement. As in Postgres, `--1` is a comment, not two minus signs.

The REPL skips a line holding only a comment, as it does a blank line,
and a statement ends at a line whose last character outside strings
and comments is `;`.

## Comparing values

`INT` values compare as numbers. `TEXT` values compare byte by byte, so
uppercase sorts before lowercase (`'B' < 'a'`) and digits compare as
characters (`'10' < '9'`). `BOOL`s compare with `false` first.

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
and `LIMIT` matters only when it's 0. `COUNT`, `SUM`, `MIN` and `MAX` are
keywords, so they can't be used as table or column names.

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
order, first with `DESC`. `IS` and `NULL` are keywords, so they can't be
used as table or column names.

## Commands

Lines starting with `.` are commands for the program, not SQL. They
skip the tokeniser and parser, need no `;`, and are recognised only at
the start of a statement.

| Command          | Does                                               |
|------------------|----------------------------------------------------|
| `.tables`        | lists the tables, in the order they were created   |
| `.schema`        | prints the `CREATE TABLE` statement for each table |
| `.schema users`  | prints it for one table                            |
| `.exit`          | quits                                              |

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

`BEGIN`, `COMMIT` and `ROLLBACK` are keywords, so they can't be used as
table or column names. `BEGIN TRANSACTION` and `END` aren't accepted.

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
- `FOR` is a keyword, so it can't be used as a table or column name.


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
  too), and a bound at each end if wanted.

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
- `INDEX` and `ON` are keywords, so they can't be used as table or
  column names.

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
any change. `ANALYZE` is a keyword, so it can't be used as a table or
column name.

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

A plan's text ends ", then sort" when it needs one, or ", in order" (and
", backwards") when it gives the `ORDER BY`'s order.

Only the table, the `WHERE` and the `ORDER BY` are checked, so a mistake
elsewhere in the statement shows up only when it runs. It needs a value
for each `?`, as running would. Unlike Postgres's `EXPLAIN`, it shows the ways
it didn't choose, too.
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

The same goes for `UPDATE` and `DELETE`. `PRIMARY`, `KEY` and `UNIQUE`
are keywords, so they can't be used as table or column names.

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
key was written. `AUTOINCREMENT` and `RETURNING` are keywords, so they
can't be used as table or column names.
