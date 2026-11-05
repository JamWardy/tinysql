# TinySQL grammar

The SQL subset the parser accepts. Each rule is written in terms of the
token types produced by the tokeniser in `src/tokeniser.h`. What each
statement then does is in `DESIGN.md`.

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
comparison    = additive [ compare_op additive | IS [ NOT ] NULL
                         | [ NOT ] ( LIKE | ILIKE ) additive
                           [ ESCAPE additive ] ]
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

## Operators

From loosest to tightest binding:

1. `OR`
2. `AND`
3. `NOT`
4. `=`, `!=` or `<>`, `<`, `<=`, `>`, `>=`, `IS [NOT] NULL`,
   `[NOT] LIKE`, `[NOT] ILIKE`
5. `+`, `-`
6. `*`, `/`, `%`
7. `-` before a value

So `a = 1 OR b = 2 AND NOT c + 1 = 3` means
`(a = 1) OR ((b = 2) AND (NOT ((c + 1) = 3)))`. Brackets group
differently. Operators of the same level group from the left, so
`10 - 3 - 2` is 5. A comparison can't be chained: `a < b < c` is an
error.

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

## Keywords

These are keywords, matched ignoring case, so they can't be used as
table or column names:

`ANALYZE`, `AND`, `AS`, `ASC`, `AUTOINCREMENT`, `BEGIN`, `BY`,
`COMMIT`, `COUNT`, `CREATE`, `DELETE`, `DESC`, `DROP`, `ESCAPE`,
`EXPLAIN`, `FALSE`, `FOR`, `FROM`, `ILIKE`, `INDEX`, `INSERT`, `INT`,
`INTO`, `IS`, `KEY`, `LIKE`, `LIMIT`, `MAX`, `MIN`, `NOT`, `NULL`, `ON`,
`OR`, `ORDER`, `PRIMARY`, `RETURNING`, `ROLLBACK`, `SELECT`, `SET`,
`SUM`, `TABLE`, `TEXT`, `TRUE`, `UNIQUE`, `UPDATE`, `VALUES`, `WHERE`.

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
SELECT * FROM items WHERE title ILIKE '%shoe%';  -- DESIGN.md: "LIKE"
SELECT * FROM items WHERE title NOT LIKE '50!%%' ESCAPE '!';
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
SELECT * FROM users WHERE age = NULL;          -- use IS NULL
SELECT * FROM users WHERE age IS 18;           -- IS only tests for NULL
SELECT * FROM users WHERE id LIKE '1%';        -- LIKE takes TEXT
SELECT * FROM users WHERE name LIKE 'a\';      -- can't end in the escape
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
depend on what tables exist, and `DESIGN.md` describes each:

- the table exists (or, for `CREATE`, doesn't already exist)
- every column an expression names exists, and each operator gets the
  types it takes
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
