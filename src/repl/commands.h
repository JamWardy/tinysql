#ifndef TINYSQL_REPL_COMMANDS_H
#define TINYSQL_REPL_COMMANDS_H

#include "storage/table.h"

// Returns 0 for .exit. Commands aren't SQL: they skip the tokeniser and parser,
// and never change the database.
int run_command(const Database *db, const char *command);

#endif
