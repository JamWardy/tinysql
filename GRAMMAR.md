# TinySQL grammar

The SQL subset the parser accepts. Each rule is written in terms of the
token types produced by the tokenizer in `src/tokenizer.h`.

## Notation

| Symbol      | Meaning                                  |
|-------------|------------------------------------------|
| `UPPERCASE` | a token type, e.g. `SELECT`, `LPAREN`    |
| `lowercase` | another rule defined in this file        |
| `a b`       | `a` followed by `b`                      |
| `a \| b`    | either `a` or `b`                        |
| `{ a }`     | `a` repeated zero or more times          |

## Rules

```
statement     = ( create_stmt | insert_stmt | select_stmt ) SEMICOLON EOF

create_stmt   = CREATE TABLE IDENTIFIER
                LPAREN column_def { COMMA column_def } RPAREN

column_def    = IDENTIFIER column_type
column_type   = INT | TEXT

insert_stmt   = INSERT INTO IDENTIFIER
                VALUES LPAREN value { COMMA value } RPAREN

value         = NUMBER | STRING

select_stmt   = SELECT select_list FROM IDENTIFIER
select_list   = STAR | IDENTIFIER { COMMA IDENTIFIER }
```

## Examples

Accepted:

```sql
CREATE TABLE users (id INT, name TEXT, age INT);
INSERT INTO users VALUES (1, 'Alice', 25);
SELECT * FROM users;
SELECT name, age FROM users;
```

Rejected:

```sql
CREATE TABLE users ();             -- needs at least one column
CREATE TABLE users (id INTEGER);   -- type must be INT or TEXT
INSERT INTO users VALUES (1, 2,);  -- trailing comma
SELECT * FROM users                -- missing semicolon
SELECT *, name FROM users;         -- '*' can't be mixed with columns
SELECT * FROM users; SELECT 1;     -- one statement per line
```

## Not checked by the grammar

The parser checks only shape. These are the executor's job, because they
depend on what tables exist:

- the table exists (or, for `CREATE`, doesn't already exist)
- column names are unique within a `CREATE TABLE`
- `INSERT` has the right number of values, of the right types
- `SELECT` names columns that exist
