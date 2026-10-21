# Fixed-size arrays vs heap in the tokenizer and parser

## Two separate choices

Memory for data involves two independent decisions:

1. **Size:** fixed in the code (`char text[128]`), or chosen while the
   program runs (`malloc(length + 1)`).
2. **Location:** where the memory comes from, which decides how long it lasts.

| | **Stack**: lasts until the function returns | **Static**: lasts the whole program | **Heap**: lasts until `free` |
|---|---|---|---|
| **Fixed size** | `char input[1024];` in a function. Here: `input` in `main`, the loader's `columns`. | `static TokenList tokens;`. Here: `tokens` and `stmt` in `main`. | `malloc(sizeof(Token))`, a struct with fixed arrays inside. Not used here. |
| **Size chosen while running** | `char buf[n];` (a variable-length array). Optional in C11 and risky: a big `n` overflows the stack with no error. Not used here. | Impossible: static memory is laid out before the program starts. | `malloc(length + 1)`. Here: every name, row and text value in storage. |

A fixed-size array lives wherever it's declared, and an array inside a
struct lives wherever the struct does. So "fixed size" and "heap" aren't
opposites.

## What the tokenizer and parser use

Their text sits in fixed-size arrays inside structs: `Token.text`,
`Statement.table_name`, `ColumnDef.name`, `Literal.as.text` and so on,
each `MAX_TOKEN_LENGTH` (128) bytes. Those structs live in static memory,
because `main` declares them as `static TokenList tokens` and
`static Statement stmt`. Storage (`src/table.c`) uses the heap instead.

## Comparison

This compares the two realistic choices for the tokenizer and parser:

- **Fixed-size arrays in static memory:** what they do now.
- **Heap, sized while running:** `malloc` each string to fit.

| Factor | Fixed-size arrays, static | Heap, sized while running | Matters most in |
|---|---|---|---|
| **Cleanup** | Nothing to free. An error just returns. | Every error path must free what's been built so far. Easy to leak. | Both, but the parser has more error paths |
| **Size limits** | Invented limits: 127 characters per token, 256 tokens, 32 columns. | No limits beyond available memory. | Tokenizer for long strings; parser for column count |
| **Out-of-memory failure** | Can't happen; the memory exists from program start. | Every `malloc` can fail, and each failure is another error path. | Both |
| **Memory waste** | Every array is full size, whether used or not: 128 bytes for `(`. `TokenList` is about 33KB even for `SELECT * FROM t;`. | Uses only what's needed, plus a little `malloc` bookkeeping per allocation. | Tokenizer, which has many small tokens |
| **Speed** | Copying into an existing array is fast, with no allocation calls. | One `malloc` per string, which is slow compared with a copy. Irrelevant at this scale. | Tokenizer, the most allocations |
| **Overflow risk** | Every copy must check the size or it writes past the end of the array, as in `add_token`'s check and `snprintf`. Forgetting one check is a classic security bug. | Allocated to the exact size, so less risk. You can still miscalculate a size, like forgetting the `+ 1` for `'\0'`. | Both |
| **Silent truncation** | `snprintf` cuts text short without complaint if a check upstream is missing. | Not an issue. | Parser, which copies with `snprintf` |
| **Copying a struct** | Copies the text too, so each copy is independent. Safe. | Copies only the pointer, so two structs share one string. Freeing both crashes the program (double free); freeing one leaves the other pointing at freed memory (use after free). | Parser, since `Statement` gets passed around |
| **Bug types you can get** | Overflows. | Leaks, double frees, use after free, and overflows. | Both |
| **Ease of use for callers** | Use the result and forget it. | The caller must call `token_list_free` or `statement_free`, and must not use the result after freeing. | Both, but mostly `main` and the executor |
| **Reuse across lines** | One copy, overwritten for each line. | Allocate and free every line, or write reset logic. | Both |
| **Static or stack** | Either works. `TokenList` is about 33KB and `Statement` about 4.5KB, well within a typical main-thread stack (8MB on macOS and Linux, 1MB on Windows). They're static as a precaution against small stacks, such as on other threads. The cost of static: there's only one copy, so the program can't work on two statements at once. | Only small pointers sit on the stack, with the data on the heap, and you can have as many copies as you like. | Tokenizer, since `TokenList` is the large one |
| **Memory locality** | Everything sits in one contiguous block, which is good for the CPU cache. | Strings are scattered around memory. Irrelevant at this scale. | Both, but negligible |
| **Debugging help** | Tools catch overflows, but there are no leaks to find. | Tools like ASan and `leaks` find leaks and misuse, but you need to run them. | Both |

## A third option

Tokens could hold a pointer into the input line plus a length, with no
copy. This keeps most of the fixed-array advantages (nothing to free,
can't run out of memory) without the length limits. Its own downside is
that tokens break if the input line changes or goes away while they're
still in use. SQLite's tokenizer works this way.
