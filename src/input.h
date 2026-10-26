#ifndef TINYSQL_INPUT_H
#define TINYSQL_INPUT_H

#include <stddef.h>

// The longest statement, all its lines included. A single line can't be
// longer either.
#define MAX_STATEMENT 4096

typedef enum {
    READ_STATEMENT,  // A complete statement is ready to run.
    READ_ERROR,      // The statement was too long; the error is printed.
    READ_EXIT,       // .exit was typed.
    READ_END         // No more input.
} ReadResult;

// Prompts for and reads lines from stdin into statement (size bytes),
// until a line ends in ';'. Statements can span lines; .exit and blank
// lines are recognised only at the start of one.
//
// A statement that doesn't fit is read to its end and dropped, so its
// last lines aren't mistaken for the start of the next statement.
ReadResult read_statement(char *statement, size_t size);

#endif
