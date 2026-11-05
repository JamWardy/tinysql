#ifndef TINYSQL_REPL_INPUT_H
#define TINYSQL_REPL_INPUT_H

#include <stddef.h>

// The longest statement, all its lines included.
#define MAX_STATEMENT 4096

typedef enum {
    READ_STATEMENT,  // A complete statement is ready to run.
    READ_ERROR,      // The statement was too long; the error is printed.
    READ_COMMAND,    // A command such as .tables is ready to run.
    READ_END         // No more input.
} ReadResult;

// Reads lines from stdin until one ends a statement with ';'. A ';' inside a
// string, which can span lines, or after a "--" comment doesn't count, so "DROP
// TABLE t; -- old" ends the statement.
//
// At the start of a statement, a line beginning with '.' is a command instead:
// it's put in statement alone, trimmed, and needs no ';'. Blank lines and
// comment-only lines are skipped there.
//
// A statement that doesn't fit is read to its end and dropped, so its last
// lines aren't taken for the next statement.
ReadResult read_statement(char *statement, size_t size);

#endif
