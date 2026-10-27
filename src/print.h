#ifndef TINYSQL_PRINT_H
#define TINYSQL_PRINT_H

#include "parser.h"
#include "result.h"

// Prints what a statement of type gave back, as the REPL shows it: a
// SELECT's rows as a table, or how many rows a DELETE or UPDATE
// changed. CREATE, INSERT and DROP print nothing.
void print_result(StatementType type, const Result *result);

#endif
