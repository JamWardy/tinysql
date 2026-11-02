#ifndef TINYSQL_REPL_INPUT_H
#define TINYSQL_REPL_INPUT_H

#include <stddef.h>

// The longest statement, all its lines included. A single line can't be
// longer either.
#define MAX_STATEMENT 4096

typedef enum {
    READ_STATEMENT,  // A complete statement is ready to run.
    READ_ERROR,      // The statement was too long; the error is printed.
    READ_COMMAND,    // A command such as .tables is ready to run.
    READ_END         // No more input.
} ReadResult;

// Prompts for and reads lines from stdin into statement (size bytes),
// until a line ends in ';'. Statements can span lines. A ';' inside a
// string, which can span lines too, or after a "--" comment doesn't
// count, so "DROP TABLE t; -- old" ends the statement.
//
// At the start of a statement, a line beginning with '.' (after any
// spaces) is a command instead: it's put in statement on its own,
// without the spaces around it or its newline, and needs no ';'. Blank
// lines and lines holding only a comment are skipped there.
//
// A statement that doesn't fit is read to its end and dropped, so its
// last lines aren't mistaken for the start of the next statement.
ReadResult read_statement(char *statement, size_t size);

#endif
