# Hash indexes vs B-trees

TinySQL's index (`src/index.c`) is a hash index. Real databases such as
SQLite, PostgreSQL and MySQL use B-trees by default. This note says how
the two differ and why a hash index is enough for the shop.

## Hash index: what TinySQL has

Each key is turned into a number, its hash, and the hash picks a slot in
an array. Finding a key means checking that slot, and perhaps the next
few if it's taken (see `index.h`).

- **Good at exact matches.** `WHERE id = 5` takes about one step, with
  10 rows or 10 million.
- **No use for order.** Hashing scatters keys on purpose, so 5, 6 and 7
  land in unrelated slots. `WHERE price < 2000`, `ORDER BY price`,
  `LIKE 'Lap%'` and "the 20 cheapest" all still read every row.

## B-tree: the alternative

A B-tree keeps keys sorted, in a tree of nodes. Each node holds many
keys, often hundreds, with a child node for each gap between them. A
search starts at the root and picks the right child at each level.

- **Exact matches take about log(n) steps.** That's a little slower than
  a hash, but the tree is shallow because each node holds so many keys:
  a million rows is about 3 levels.
- **Ranges and order are fast.** Find where a range starts, then read on
  in order. `BETWEEN`, `<`, `ORDER BY ... LIMIT` and `LIKE 'abc%'` (the
  names starting "abc" sit together) all use it.
- **Built for disks.** One node fills one disk page, so each level costs
  one read. That's why databases that keep data on disk use them;
  SQLite stores whole tables as B-trees, not only indexes.

## Side by side

| | Hash index | B-tree |
|---|---|---|
| `WHERE id = 5` | About 1 step | About log(n) steps |
| `WHERE price < 2000` | Reads every row | Fast |
| `ORDER BY` and `LIMIT` | Must sort | Already in order |
| `LIKE 'abc%'` | Reads every row | Fast |
| Writing one | About 150 lines (`index.c`) | Much harder: nodes split when full and merge when emptied |

## Why the shop doesn't need a B-tree

The shop's queries (see `../TODO.md`) are almost all exact matches:

- Product page: `WHERE id = ?`
- Logging in: `WHERE email = ?`, with a `UNIQUE` index
- Basket and order history: `WHERE user_id = ?`, with `CREATE INDEX`

The ones that sort, such as a category's products by price or a user's
orders by date, sort only the few rows the hash index found. With a few
dozen rows, that's instant.

So the hash index covers everything the shop needs, and `UNIQUE` and
`CREATE INDEX` can reuse `index.c`. A B-tree would only pay off for
range queries over large tables. It's a good project later for learning
how real databases store data, but not a step towards the shop.
