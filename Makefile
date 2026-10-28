CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g

# The database itself, which other programs link in as libtinysql.a
# and use through src/tinysql.h.
LIB_SRCS = src/tinysql.c src/tokeniser.c src/parser.c src/table.c \
           src/index.c src/executor.c src/result.c src/persist.c \
           src/error.c src/text.c
# The REPL: reading input, printing results and the .tables and .schema
# commands.
APP_SRCS = src/main.c src/input.c src/print.c src/commands.c

LIB_OBJS = $(LIB_SRCS:.c=.o)
APP_OBJS = $(APP_SRCS:.c=.o)
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
src/%.o: src/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f tinysql libtinysql.a $(LIB_OBJS) $(APP_OBJS) $(DEPS)

-include $(DEPS)

.PHONY: clean
