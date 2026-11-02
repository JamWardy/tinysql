#ifndef TINYSQL_REPL_PRINT_H
#define TINYSQL_REPL_PRINT_H

#include "result.h"

// Prints what a statement gave back, as the REPL shows it: a SELECT's
// rows as a table, or how many rows a DELETE or UPDATE changed. CREATE,
// INSERT and DROP print nothing.
void print_result(const Result *result);

#endif
