#ifndef TINYSQL_COMMANDS_H
#define TINYSQL_COMMANDS_H

#include "table.h"

// Runs a command such as ".tables" or ".schema users", as read by
// read_statement, printing its output or an error. Returns 0 if the
// command was .exit, otherwise 1.
//
// Commands aren't SQL: they're handled here, without the tokeniser or
// parser, and never change the database.
int run_command(const Database *db, const char *command);

#endif
