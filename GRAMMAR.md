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
                | update_stmt | drop_stmt ) SEMICOLON EOF

create_stmt   = CREATE TABLE IDENTIFIER
                LPAREN table_element { COMMA table_element } RPAREN
table_element = column_def | table_key

column_def    = IDENTIFIER column_type { column_constraint }
column_type   = INT | TEXT
column_constraint = NOT NULL | PRIMARY KEY | UNIQUE
table_key     = ( PRIMARY KEY | UNIQUE )
                LPAREN IDENTIFIER { COMMA IDENTIFIER } RPAREN

insert_stmt   = INSERT INTO IDENTIFIER
                [ LPAREN IDENTIFIER { COMMA IDENTIFIER } RPAREN ]
                VALUES LPAREN value { COMMA value } RPAREN

value         = [ MINUS ] NUMBER | STRING | NULL | PARAM

select_stmt   = SELECT select_list FROM IDENTIFIER [ WHERE condition ]
                [ ORDER BY IDENTIFIER [ ASC | DESC ] ]
                [ LIMIT ( NUMBER | PARAM ) ]
select_list   = STAR
              | aggregate { COMMA aggregate }
              | IDENTIFIER { COMMA IDENTIFIER }
aggregate     = COUNT LPAREN ( STAR | IDENTIFIER ) RPAREN
              | ( SUM | MIN | MAX ) LPAREN IDENTIFIER RPAREN

delete_stmt   = DELETE FROM IDENTIFIER [ WHERE condition ]

update_stmt   = UPDATE IDENTIFIER
                SET assignment { COMMA assignment } [ WHERE condition ]
assignment    = IDENTIFIER EQUAL value

drop_stmt     = DROP TABLE IDENTIFIER

condition     = and_condition { OR and_condition }
and_condition = not_condition { AND not_condition }
not_condition = NOT not_condition
              | LPAREN condition RPAREN
              | comparison
comparison    = IDENTIFIER compare_op value
              | IDENTIFIER IS [ NOT ] NULL
compare_op    = EQUAL | NOT_EQUAL | LESS | LESS_EQUAL
              | GREATER | GREATER_EQUAL
```

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
INSERT INTO users VALUES (- -1);   -- only one '-'
INSERT INTO users VALUES (-'a');   -- '-' only before a number
INSERT INTO users VALUES (-NULL);  -- or NULL
INSERT INTO users VALUES (4, 'O'Brien', 40);  -- write the quote as ''
INSERT INTO users () VALUES ();    -- name at least one column
INSERT INTO users (id, name) VALUES (1);  -- one value per column
SELECT * FROM users                -- missing semicolon
SELECT *, name FROM users;         -- '*' can't be mixed with columns
SELECT * FROM users; SELECT 1;     -- one statement at a time
SELECT * FROM users WHERE 18 < age;   -- column must come first
SELECT * FROM users WHERE age = NULL;          -- use IS NULL (see below)
SELECT * FROM users WHERE age IS 18;           -- IS only tests for NULL
SELECT * FROM users WHERE age > 1 AND;         -- AND needs a right side
SELECT * FROM users WHERE (age > 1;            -- unclosed '('
SELECT * FROM users LIMIT 3 ORDER BY age;      -- ORDER BY comes first
SELECT * FROM users ORDER BY age, name;        -- one sort column only
SELECT name, COUNT(*) FROM users;              -- no mixing (no GROUP BY)
SELECT * FROM users LIMIT -1;                  -- LIMIT can't be negative
DELETE users WHERE id = 1;                     -- needs FROM
DELETE * FROM users;                           -- no column list
UPDATE users age = 26;                         -- needs SET
UPDATE users SET age = age + 1;                -- values only, no expressions
DROP users;                                    -- needs TABLE
DROP TABLE a, b;                               -- one table at a time
```

## Not checked by the grammar

The parser checks only shape. These are the executor's job, because they
depend on what tables exist:

- the table exists (or, for `CREATE`, doesn't already exist)
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
- `SELECT` names columns that exist, including the `ORDER BY` column
  and the columns inside aggregates
- `SUM` is given an `INT` column
- the `WHERE` columns exist, in `SELECT`, `UPDATE` and `DELETE`
- each `WHERE` value has the same type as its column
- there's one value for each `?`, and a `LIMIT ?`'s is a non-negative
  `INT`

## Inserting

An `INSERT` gives a value for every column, in the table's order, or
names the columns it gives values for, in any order:

```sql
INSERT INTO users VALUES (1, 'Alice', 25);
INSERT INTO users (name, id) VALUES ('Ann', 5);
```

A column left out is `NULL`, so it can't be `NOT NULL`. There must be
one value per named column, and each column is named at most once.
`()` with no columns is a syntax error.

## Combining conditions

From tightest to loosest binding: `NOT`, then `AND`, then `OR`. So

```sql
WHERE a = 1 OR b = 2 AND NOT c = 3
```

means `a = 1 OR (b = 2 AND (NOT c = 3))`. Use parentheses to group
differently. Several `AND`s or `OR`s in a row group from the left.

A `WHERE` can hold at most 64 nodes, counting each comparison, `AND`, `OR`
and `NOT` as one. Parentheses don't count.

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
characters (`'10' < '9'`).

## Ordering and limiting

`SELECT` filters with `WHERE`, then sorts with `ORDER BY`, then keeps
the first `LIMIT` rows. Without `ORDER BY`, rows come out in the order
they were inserted. Rows with equal sort values also keep that order.
The `ORDER BY` column doesn't have to be one of the selected columns.

## Aggregates

`COUNT`, `SUM`, `MIN` and `MAX` read every row that matches the
`WHERE` and give a single result row. `COUNT(*)` counts those rows.
`SUM` adds an `INT` column, and fails if the total doesn't fit in one.
`MIN` and `MAX` take either type, comparing values as described above.

`COUNT(*)` counts rows, including ones with `NULL`s. `COUNT(column)`
counts only the rows where that column isn't `NULL`. `SUM`, `MIN` and
`MAX` skip `NULL`s, and give `NULL` when there's nothing else to use: no
matching rows, or only `NULL`s. `COUNT` gives 0 instead.

There's only ever one result row, so `ORDER BY` changes nothing and
`LIMIT` matters only when it's 0. `COUNT`, `SUM`, `MIN` and `MAX` are
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
