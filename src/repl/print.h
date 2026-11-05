#ifndef TINYSQL_REPL_PRINT_H
#define TINYSQL_REPL_PRINT_H

#include "result.h"

// Rows as a table, or how many rows a DELETE or UPDATE changed. Statements that
// give back nothing print nothing.
void print_result(const Result *result);

#endif
