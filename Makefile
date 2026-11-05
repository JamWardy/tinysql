CC ?= cc
# -Isrc lets files in src/'s folders include src/'s headers by name.
# compile_flags.txt repeats -std and -I for clangd, the editor's checker.
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -Isrc

# The database itself, which other programs link in as libtinysql.a
# and use through src/tinysql.h.
LIB_SRCS = src/tinysql.c src/tokeniser.c src/table.c src/index.c \
           src/result.c src/persist.c src/transaction.c src/error.c src/text.c \
           src/value.c src/btree.c \
           $(PARSER_SRCS) $(EXECUTOR_SRCS)
# The parser, one file per statement plus what they share.
PARSER_SRCS = src/parser/parser.c src/parser/common.c \
              src/parser/expr.c src/parser/deparse.c \
              src/parser/schema.c \
              src/parser/insert.c src/parser/select.c \
              src/parser/update.c src/parser/delete.c \
              src/parser/transaction.c
# The executor, one file per statement plus what they share.
EXECUTOR_SRCS = src/executor/executor.c src/executor/common.c \
                src/executor/expr.c src/executor/where.c \
                src/executor/schema.c \
                src/executor/insert.c src/executor/select.c \
                src/executor/update.c src/executor/delete.c
# The REPL: reading input, printing results and the .tables and .schema
# commands.
APP_SRCS = src/repl/main.c src/repl/input.c src/repl/print.c \
           src/repl/commands.c

# Objects and .d files go in build/, mirroring src/.
LIB_OBJS = $(LIB_SRCS:src/%.c=build/%.o)
APP_OBJS = $(APP_SRCS:src/%.c=build/%.o)
DEPS = $(LIB_OBJS:.o=.d) $(APP_OBJS:.o=.d)

tinysql: $(APP_OBJS) libtinysql.a
	$(CC) $(APP_OBJS) libtinysql.a -o $@

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
	rm -rf tinysql libtinysql.a build

-include $(DEPS)

.PHONY: clean
