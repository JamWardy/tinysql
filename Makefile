CC ?= cc
# -Isrc lets files in src/'s folders include src/'s headers by name.
# compile_flags.txt repeats -std and -I for clangd, the editor's checker.
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -Isrc

# The database itself, which other programs link in as libtinysql.a
# and use through src/tinysql.h.
LIB_SRCS = src/tinysql.c src/tokeniser.c src/result.c src/error.c \
           src/text.c src/value.c src/like.c \
           $(STORAGE_SRCS) $(PARSER_SRCS) $(EXECUTOR_SRCS)
# Tables in memory, their indexes, the file they're saved to and its lock.
STORAGE_SRCS = src/storage/table.c src/storage/index.c src/storage/btree.c \
               src/storage/persist.c src/storage/transaction.c \
               src/storage/lock.c
# The parser, one file per statement plus what they share.
PARSER_SRCS = src/parser/parser.c src/parser/common.c \
              src/parser/expr.c src/parser/deparse.c \
              src/parser/schema.c \
              src/parser/insert.c src/parser/select.c \
              src/parser/update.c src/parser/delete.c \
              src/parser/transaction.c src/parser/analyze.c
# The executor, one file per statement plus what they share.
EXECUTOR_SRCS = src/executor/executor.c src/executor/common.c \
                src/executor/expr.c src/executor/where.c \
                src/executor/schema.c \
                src/executor/insert.c src/executor/select.c \
                src/executor/update.c src/executor/delete.c \
                src/executor/plan.c src/executor/explain.c \
                src/executor/analyze.c
# The REPL: reading input, printing results and the .tables and .schema
# commands.
APP_SRCS = src/repl/main.c src/repl/input.c src/repl/print.c \
           src/repl/commands.c
# The server: its loop, the messages it swaps with clients, and replication,
# from the primary's side and the replica's.
SERVER_SRCS = src/server/main.c src/server/server.c src/server/protocol.c \
              src/server/primary.c src/server/replica.c

# Objects and .d files go in build/, mirroring src/.
LIB_OBJS = $(LIB_SRCS:src/%.c=build/%.o)
APP_OBJS = $(APP_SRCS:src/%.c=build/%.o)
SERVER_OBJS = $(SERVER_SRCS:src/%.c=build/%.o)
DEPS = $(LIB_OBJS:.o=.d) $(APP_OBJS:.o=.d) $(SERVER_OBJS:.o=.d)

all: tinysql tinysql-server

tinysql: $(APP_OBJS) libtinysql.a
	$(CC) $(APP_OBJS) libtinysql.a -o $@

tinysql-server: $(SERVER_OBJS) libtinysql.a
	$(CC) $(SERVER_OBJS) libtinysql.a -o $@

# ar bundles the objects into one archive; r adds or replaces them, c
# creates the archive without a warning, and s writes an index of its
# symbols for the linker.
libtinysql.a: $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJS)

# -MMD -MP write a .d file per object listing the headers it includes,
# so editing a header rebuilds every .c file that uses it.
build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf tinysql tinysql-server libtinysql.a build

-include $(DEPS)

.PHONY: all clean
