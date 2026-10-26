#ifndef TINYSQL_INPUT_H
#define TINYSQL_INPUT_H

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
// until a line ends in ';'. Statements can span lines.
//
// At the start of a statement, a line beginning with '.' (after any
// spaces) is a command instead: it's put in statement on its own,
// without the spaces around it or its newline, and needs no ';'. Blank
// lines there are skipped.
//
// A statement that doesn't fit is read to its end and dropped, so its
// last lines aren't mistaken for the start of the next statement.
ReadResult read_statement(char *statement, size_t size);

#endif
