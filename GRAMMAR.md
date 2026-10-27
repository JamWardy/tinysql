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
                LPAREN column_def { COMMA column_def } RPAREN

column_def    = IDENTIFIER column_type
column_type   = INT | TEXT

insert_stmt   = INSERT INTO IDENTIFIER
                VALUES LPAREN value { COMMA value } RPAREN

value         = [ MINUS ] NUMBER | STRING | NULL

select_stmt   = SELECT select_list FROM IDENTIFIER [ WHERE condition ]
                [ ORDER BY IDENTIFIER [ ASC | DESC ] ] [ LIMIT NUMBER ]
select_list   = STAR
              | aggregate { COMMA aggregate }
              | IDENTIFIER { COMMA IDENTIFIER }
aggregate     = COUNT LPAREN STAR RPAREN
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
INSERT INTO users VALUES (1, 'Alice', 25);
INSERT INTO users VALUES (-3, 'Eve', 30);
INSERT INTO users VALUES (2, NULL, NULL);    -- any column can be NULL
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
DELETE FROM users WHERE age < 18;
DELETE FROM users;                            -- deletes every row
UPDATE users SET age = 26 WHERE name = 'Alice';
UPDATE users SET name = 'Anon', age = 0;      -- updates every row
UPDATE users SET age = NULL WHERE id = 2;
DROP TABLE users;
```

Rejected:

```sql
CREATE TABLE users ();             -- needs at least one column
CREATE TABLE users (id INTEGER);   -- type must be INT or TEXT
INSERT INTO users VALUES (1, 2,);  -- trailing comma
INSERT INTO users VALUES (--1);    -- only one '-'
INSERT INTO users VALUES (-'a');   -- '-' only before a number
INSERT INTO users VALUES (-NULL);  -- or NULL
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
SELECT COUNT(id) FROM users;                   -- only COUNT(*)
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
- `INSERT` has the right number of values, of the right types (`NULL`
  fits any column)
- `UPDATE` sets columns that exist, each at most once, to values of the
  right types or `NULL`
- `SELECT` names columns that exist, including the `ORDER BY` column
  and the columns inside aggregates
- `SUM` is given an `INT` column
- the `WHERE` columns exist, in `SELECT`, `UPDATE` and `DELETE`
- each `WHERE` value has the same type as its column

## Combining conditions

From tightest to loosest binding: `NOT`, then `AND`, then `OR`. So

```sql
WHERE a = 1 OR b = 2 AND NOT c = 3
```

means `a = 1 OR (b = 2 AND (NOT c = 3))`. Use parentheses to group
differently. Several `AND`s or `OR`s in a row group from the left.

A `WHERE` can hold at most 64 nodes, counting each comparison, `AND`, `OR`
and `NOT` as one. Parentheses don't count.

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

`COUNT(*)`, `SUM`, `MIN` and `MAX` read every row that matches the
`WHERE` and give a single result row. `COUNT(*)` counts those rows.
`SUM` adds an `INT` column, and fails if the total doesn't fit in one.
`MIN` and `MAX` take either type, comparing values as described above.

`COUNT(*)` counts rows, including ones with `NULL`s. `SUM`, `MIN` and
`MAX` skip `NULL`s, and give `NULL` when there's nothing else to use: no
matching rows, or only `NULL`s. With no matching rows, `COUNT(*)` gives 0.

There's only ever one result row, so `ORDER BY` changes nothing and
`LIMIT` matters only when it's 0. `COUNT`, `SUM`, `MIN` and `MAX` are
keywords, so they can't be used as table or column names.

## NULL

`NULL` means "no value": unknown or missing, such as an age nobody gave.
It isn't 0 or `''`, and any column can hold it. Results print it as
`NULL`, so it doesn't look the same as an empty string.

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
