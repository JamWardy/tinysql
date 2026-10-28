# TinySQL code conventions

How the code is written, so it stays consistent as it grows. Each rule
says why, because a rule without a reason gets broken the first time it's
inconvenient. When code breaks a rule on purpose, a comment says why.

Related notes: [FIXED_ARRAYS_VS_HEAP.md](FIXED_ARRAYS_VS_HEAP.md) for
where memory comes from, [C_LIBRARY.md](C_LIBRARY.md) for the library
functions we call, [GRAMMAR.md](GRAMMAR.md) for the SQL we accept, and
[INDEXES.md](INDEXES.md) for why the index is a hash and not a B-tree.

## Functions

- **One job per function.** If its comment needs "and then", or its body
  falls into steps that each want a heading comment, split it. For
  example, `execute_select` only finds the table and picks
  `select_rows` or `select_aggregates`.
- **Check everything, then act.** Look up every table and column and
  check every type before reading a row, printing or changing anything,
  as `select_rows` and `execute_update` do. A bad statement then fails
  with nothing half done.
- **Extract a pattern the third time you write it.** That's how
  `find_column`, `find_table`, `check_type`, `fail` and `copy_text` came
  about. Name the helper after what it means, not how it works.
- **`static` unless another file needs it.** A public function goes in
  the header with a comment saying what it does, what it returns and
  what happens on failure.

## Errors

A function that can fail does one of two things:

| It produces | Returns | On failure |
|---|---|---|
| A yes or no, or results through out parameters | `int`: 1 for success | Returns 0 after writing a message to `char *error, size_t error_size` |
| One pointer | The pointer | Returns `NULL`, after writing a message if it takes an `error` buffer (`find_table`) |

- **Write errors with `fail`**, which is one line:
  `return fail(error, error_size, "no such table '%s'", name);`.
  The loader uses `read_fail`, which writes into its `Reader` instead.
- **Message style:** lowercase, no full stop at the end, names in single
  quotes. For example: `column 'age' is INT but the value is TEXT`.
- **`assert` is for bugs in TinySQL only**, never for bad SQL or a bad
  file. Those are the user's input and get an error message.
- **The database file is untrusted.** Check every count, length and type
  in it before using it, as `persist.c` does.

## Pointers and ownership

- **`const` on every pointer the function only reads.** Then the
  signature alone tells you what the function might change.
- **The `Database` owns all heap memory reachable from it.** Functions
  that store strings or values copy them, as `table.h` explains. Code
  outside `table.c` never frees a table's memory.
- **A borrowed pointer is labelled.** When something points into memory
  it doesn't own, such as the executor's `Row *`s into the table, the
  comment says so and says how long it stays valid.
- **Prefer returning a value to an out parameter.** Use an out parameter
  only when the return value is already the success flag.
- **At most two `*` in a parameter.** A `***` means an array is being
  returned through an out parameter; return a struct instead.
- **An array travels with its count.** Put them in one struct, such as
  the executor's `ColumnList` and `RowList` or the parser's
  `CreateStatement`, rather than passing two parameters that must agree.
- **A buffer travels with its size**, either as a `size_t size`
  parameter (`copy_text`, `fail`, `read_name`) or fixed by its type
  (`ColumnList`, `NullBitmap`). Never rely on the caller just knowing
  it's big enough.
- **Copy before freeing.** When replacing heap memory, build the new
  value first and free the old one only once that worked, as
  `table_set_value` does. Running out of memory then changes nothing.
- **`realloc` into a new variable**, so the old pointer survives a
  failure. Remember that `malloc(0)` may return `NULL` without failing.

## Memory locations

See [FIXED_ARRAYS_VS_HEAP.md](FIXED_ARRAYS_VS_HEAP.md) for the
reasoning. In short:

| Use | When |
|---|---|
| Stack | The default, for anything that's only needed until the function returns |
| Static | Only for something too big for the stack that's reused (`tokens` and `stmt` in `tinysql_run`), or when a library leaves no other way (`sort_by`, for `qsort`). Comment why |
| Heap | Data whose size is only known while running, or that outlives the function. Every `malloc` has one clear owner who frees it |

## Values and types

- **Counts, sizes and indexes are `size_t`** in new code. The parser's
  structs still use `int`; cast where the two meet rather than spreading
  `int` further.
- **"There isn't one" is a flag**, `has_where`, `has_order`, alongside
  the value, not a special value such as `-1`. A lookup that may find
  nothing returns 1 or 0 and writes what it found through a pointer, as
  `table_find_column` does. A `NULL` pointer is fine
  for an optional pointer, such as `resolve_where`'s filter.
- **No `default:` in a switch over our own enums.** List every case, so
  the compiler warns about each switch that needs updating when a value
  is added, as happened when `TYPE_NULL` was added. Switches over token
  types in the parser use `default:` for "anything else", which is fine.
- **Check before arithmetic that could overflow**, as `SUM` does. In C,
  signed overflow is undefined behaviour, not a wrap-around.

## Library calls

- **Use our wrappers** where one exists: `fail` instead of `snprintf`
  plus `return 0`, and `copy_text` or `copy_slice` instead of copying a
  string with `snprintf` or `memcpy`.
- **Wrap a library call when the wrapper names the intent or removes
  repetition**, not every call. `printf` in a print function is already
  clear.
- **Using a library function the code doesn't use yet?** Add it to
  [C_LIBRARY.md](C_LIBRARY.md).

## Layout and naming

- 4 spaces, no tabs. Lines at most 80 characters. Braces on every `if`,
  `else`, `for` and `while`, even for one line.
- `//` comments only, written as sentences. Comments say why, or what a
  caller needs to know; the code says what.
- Functions and variables `snake_case`; types `PascalCase`; constants,
  macros and enum values `UPPER_CASE`, with a prefix per enum (`TYPE_`,
  `TOKEN_`, `CMP_`).
- Public functions start with what they act on, where there is one:
  `database_find_table`, `table_append_row`.
- Header guards are `TINYSQL_<FILE>_H`.
- Includes: the file's own header, then `<system>` headers, then
  `"project"` headers, each group alphabetical.
- British spelling in comments, messages, docs and new names
  (`tokeniser`, `behaviour`). Existing names stay as they are.

## Making a change

- **Build with no warnings.** The `Makefile` uses
  `-Wall -Wextra -Wpedantic`; treat each warning as a bug.
- **Try it by hand**, including the error cases: a missing table, a
  wrong type, input cut short. There's no test suite, by choice.
- **For memory changes**, also run with `-fsanitize=address,undefined`,
  or where AddressSanitizer doesn't run, UBSan plus
  `MallocScribble=1 MallocErrorAbort=1` on macOS.
- **Keep the docs in step:** `GRAMMAR.md` for any SQL change, `TODO.md`
  when something is done, `C_LIBRARY.md` for new library functions.
- **Changing the file format?** Update the layout comment in
  `persist.c`, bump `FORMAT_VERSION`, and keep loading every older
  version.
- **Commits** go on `main`, one logical change each, and each one
  builds. The subject says what changed, in the imperative ("Add",
  "Split"); the body says why.

## Not following these yet

Known exceptions, to fix rather than copy. None at the moment.

Remove each line when it's fixed.
