# C standard library functions used in TinySQL

Every function here comes with the C compiler. Each is declared in a
header, the `<...>` names at the top of a `.c` file, and you only get the
functions whose header you include. For the full details of any of them,
run `man 3 <name>`, e.g. `man 3 snprintf`.

## Our own wrappers

Where a library call kept being written the same way, it's wrapped in a
helper. Prefer these to the raw calls.

| Helper | Where | Wraps | Use it to |
|---|---|---|---|
| `fail` | `src/error.h` | `vsnprintf` | Write an error message and return 0 in one line |
| `copy_text` | `src/text.h` | `strlen`, `memcpy` | Copy a string into a fixed-size array |
| `copy_slice` | `src/text.h` | `memcpy` | Copy part of a string, e.g. one word, into a fixed-size array |
| `copy_string` | `src/table.c` | `malloc`, `memcpy` | Copy a string onto the heap (C11 has no `strdup`) |
| `read_fail` | `src/persist.c` | `vsnprintf` | `fail` for the loader, which keeps its error buffer in a `Reader` |
| `make_temp_path` | `src/persist.c` | `malloc`, `memcpy` | Build `path` + `".tmp"` for saving |
| `discard_temp` | `src/persist.c` | `remove`, `free` | Clean up after a failed save |

## Patterns that make the calls look strange

- **Failure is a return value.** C has no exceptions. A library call that
  can fail says so by returning `NULL`, `EOF`, `-1` or nonzero, and
  nothing happens unless the caller checks. Our functions follow the
  same rule: they return 0 and write a message to `error`.
- **Strings end in `'\0'`.** A C string is just `char`s followed by a
  zero byte, and the library finds the end by searching for it. That's
  why sizes have a `+ 1`, and why an array of 128 `char`s holds at most
  127 characters.
- **The caller passes the buffer and its size.** Functions such as
  `snprintf` and `fgets` don't allocate. They write into your array and
  need its size so they don't write past the end.
- **`sizeof(array)` only works on the array itself.** Inside the function
  that declares `char name[128]`, `sizeof(name)` is 128. Pass `name` to
  another function and it arrives as a `char *`, where `sizeof` gives the
  pointer's size (8). That's why sizes travel as a separate `size_t`.
- **`(unsigned char)` before `<ctype.h>` calls.** See [below](#ctypeh-classifying-characters).
- **`errno = 0` before `strtoll`.** See [below](#errnoh-why-a-call-failed).

## `<stdio.h>`: input, output and files

| Function | What it does | Used in |
|---|---|---|
| `printf(format, ...)` | Prints formatted text to the terminal (stdout). | `executor.c`, `commands.c`, `main.c`, `input.c` |
| `fprintf(stream, format, ...)` | `printf` to a chosen stream. We use `stderr` for errors before the prompt starts. | `main.c` |
| `puts(s)` | Prints `s` and a newline. | `commands.c`, `main.c`, `input.c` |
| `putchar(c)` | Prints one character. | `executor.c` |
| `snprintf(buf, size, format, ...)` | `printf` into `buf` instead of the terminal. Writes at most `size - 1` characters plus `'\0'`, cutting the text short if needed. Returns the length the full text *would* have had. | `executor.c`, `parser.c` |
| `vsnprintf(buf, size, format, args)` | `snprintf` taking a `va_list` instead of `...`. It's how one variadic function passes its arguments on to another. | `error.c`, `persist.c` |
| `fgets(buf, size, stream)` | Reads one line, including its `'\n'`, into `buf`. Stops early if `buf` fills up, leaving the rest of the line unread. Returns `NULL` at end of input. | `input.c` |
| `getchar()` | Reads one character from stdin. Returns `EOF` at end of input, so the result is an `int`, not a `char`. | `input.c` |
| `fflush(stream)` | Sends buffered output now. The `sql> ` prompt has no newline, so without this it might not appear before we wait for input. | `input.c` |
| `fopen(path, mode)` | Opens a file and returns a `FILE *`, or `NULL` with `errno` set. Mode `"rb"` reads, `"wb"` creates or empties then writes. The `b` means binary: bytes are left exactly as they are. | `persist.c` |
| `fclose(file)` | Closes a file. It also writes out anything still buffered, so it can fail too. | `persist.c` |
| `fread(buf, 1, n, file)` | Reads `n` bytes. Returns how many it got, which is fewer at end of file or on an error. | `persist.c` |
| `fwrite(buf, 1, n, file)` | Writes `n` bytes. | `persist.c` |
| `fputc(c, file)` | Writes one byte. | `persist.c` |
| `fseek(file, offset, whence)` | Moves the read position, to the end with `SEEK_END` or the start with `SEEK_SET`. | `persist.c` |
| `ftell(file)` | Returns the read position. After seeking to the end, that's the file's size. | `persist.c` |
| `ferror(file)` | Nonzero if any earlier read or write on `file` failed. Save writes everything, then checks this once. | `persist.c` |
| `rename(old, new)` | Renames a file, replacing `new` if it exists. Save writes to `x.db.tmp`, then renames it to `x.db` so the swap happens in one step. | `persist.c` |
| `remove(path)` | Deletes a file. | `persist.c` |

`stdin`, `stdout` and `stderr` are the three streams every program starts
with: keyboard input, normal output and error output. `EOF` is the
negative value input functions return when there's nothing left to read.

### Format strings

`printf`, `snprintf` and friends replace each `%` code in the format with
the next argument. The type must match, which the compiler checks for
you. The codes we use:

| Code | Prints | Example |
|---|---|---|
| `%s` | A string | `"no such table '%s'"` |
| `%d` | An `int` | `"too many columns (max %d)"` |
| `%c` | One character | `"unexpected character '%c'"` |
| `%zu` | A `size_t` | `"(%zu rows)"` |
| `%lld`, `%llu` | A `long long`, an `unsigned long long` | Integer values, file counts |
| `%.20s` | At most 20 characters of a string | The start of a token that's too long |
| `%*lld`, `%-*s` | Padded to a width given as an extra `int` argument; `-` pads on the right | Lining up result columns |

## `<string.h>`: strings and raw memory

| Function | What it does | Used in |
|---|---|---|
| `strlen(s)` | Counts the characters before the `'\0'`. | `executor.c`, `input.c`, `persist.c`, `table.c`, `text.c` |
| `strcmp(a, b)` | Compares two strings: 0 if equal, negative if `a` sorts first, positive if `b` does. Always compare the result with 0; `if (strcmp(a, b))` means "if *different*". | `executor.c`, `commands.c`, `persist.c`, `table.c` |
| `strncmp(a, b, n)` | `strcmp` on at most the first `n` characters. | `tokeniser.c`, for two-character operators such as `<=` |
| `memcpy(dest, src, n)` | Copies `n` bytes. Doesn't care about `'\0'`, so it works on any data. `dest` and `src` mustn't overlap. | `text.c`, `table.c`, `persist.c`, `input.c`, `executor.c` |
| `memmove(dest, src, n)` | `memcpy` that's safe when they overlap. Used to shift array elements down one place after a removal. | `table.c` |
| `memset(p, 0, n)` | Sets `n` bytes to zero, e.g. to clear a struct. | `table.c`, `persist.c` |
| `memcmp(a, b, n)` | `strcmp` for raw bytes: compares exactly `n` bytes, ignoring `'\0'`. | `persist.c`, to check a file starts with `TSQL` |
| `memchr(p, c, n)` | Finds the first byte equal to `c` in the first `n` bytes, or returns `NULL`. | `persist.c`, to reject a `'\0'` inside a stored string |
| `strerror(errnum)` | The message for an error code, e.g. `"Permission denied"`. | `persist.c` |

## `<stdlib.h>`: heap memory, numbers and sorting

| Function | What it does | Used in |
|---|---|---|
| `malloc(n)` | Allocates `n` bytes on the heap. Returns `NULL` if memory ran out. The bytes start with whatever was there before. | `executor.c`, `persist.c`, `table.c` |
| `calloc(count, size)` | Allocates room for `count` items of `size` bytes, all set to zero. | `table.c`, `index.c`, `executor.c` |
| `realloc(p, n)` | Resizes an allocation. It may move the data, so it returns the new address, and on failure it returns `NULL` but leaves `p` as it was. That's why the result goes into a new variable before replacing the old pointer. | `table.c` |
| `free(p)` | Gives an allocation back. Using `p` afterwards is a bug. `free(NULL)` does nothing. | `executor.c`, `index.c`, `persist.c`, `table.c` |
| `strtoll(text, end, base)` | Converts text to a `long long`. On overflow it returns the largest value and sets `errno` to `ERANGE`. | `parser.c` |
| `qsort(array, count, size, compare)` | Sorts any array, calling `compare(a, b)` on pairs of elements. `compare` returns negative, 0 or positive like `strcmp`. | `executor.c`, for `ORDER BY` |

`malloc(0)` and `calloc(0, ...)` may return `NULL` without having
failed, so the code checks for that case where a size can be 0.

## `<ctype.h>`: classifying characters

| Function | True for |
|---|---|
| `isspace(c)` | Space, tab, newline and other blank characters |
| `isalpha(c)` | A letter |
| `isdigit(c)` | `0` to `9` |
| `isalnum(c)` | A letter or digit |
| `toupper(c)` | Not a test: returns the upper-case version of a letter, or `c` unchanged |

These take an `int` that must be either `EOF` or a value from 0 to 255.
`char` may be signed, so a byte above 127 (in `é`, say) can arrive as a
negative number, which they don't allow. Hence the cast
`isspace((unsigned char)c)`. Used in `tokeniser.c`, `input.c` and
`commands.c`.

## `<stdarg.h>`: functions that take `...`

A function declared with `...`, like `fail`, can take any number of
extra arguments. It reads them with:

| Name | What it does |
|---|---|
| `va_list` | The type holding the position in the extra arguments |
| `va_start(args, last)` | Starts reading, after the named parameter `last` |
| `va_end(args)` | Finishes. Must be called before returning |

You can't pass `...` straight on to `snprintf`, so `fail` passes the
`va_list` to `vsnprintf` instead. Used in `error.c` and `persist.c`.

## `<errno.h>`: why a call failed

`errno` is a global `int` that many library functions set when they
fail. It isn't reset on success, so to be sure a nonzero value came from
a particular call, set `errno = 0` just before it. We check:

| Code | Meaning | Where |
|---|---|---|
| `ERANGE` | A number didn't fit | After `strtoll` in `parser.c` |
| `ENOENT` | No such file | After `fopen` in `persist.c`: no database file yet, so start empty |

Read `errno` straight after the call that failed. Another call in
between, even `remove`, may change it.

## `<assert.h>`: checking our own assumptions

`assert(condition)` stops the program with a message naming the file
and line if `condition` is false. It's for things that can only go wrong
through a bug in TinySQL, such as an index past the end of an array. It
isn't for bad input from the user or the file; those get an error
message. Used in `index.c`, `parser.c`, `persist.c`, `table.c` and
`text.c`.

## Types and limits

| Name | Header | What it is |
|---|---|---|
| `size_t` | `<stddef.h>` | An unsigned integer for sizes and counts. It's what `sizeof` and `strlen` give |
| `NULL` | `<stddef.h>` | The "points nowhere" pointer. Nothing to do with SQL's `NULL`, which a `Value` stores as `TYPE_NULL` |
| `LLONG_MAX`, `LLONG_MIN` | `<limits.h>` | The largest and smallest `long long`. `SUM` checks against them before adding, and the loader uses `LLONG_MAX` to turn stored integers back into signed ones |
| `uint64_t` | `<stdint.h>` | An unsigned integer of exactly 64 bits, for the file format and index hashes |
| `UINT32_MAX` | `<stdint.h>` | The largest 32-bit unsigned value, the most a string length in the file can be |
| `UINT8_MAX` | `<stdint.h>` | The largest 8-bit unsigned value, the most constraints a table's count byte in the file can hold |
| `SIZE_MAX` | `<stdint.h>` | The largest `size_t`, used to check an allocation's size in bytes fits before growing an index |
