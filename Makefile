CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g

SRCS = src/main.c src/tokeniser.c src/parser.c src/table.c src/executor.c \
       src/persist.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

tinysql: $(OBJS)
	$(CC) $(OBJS) -o $@

# -MMD -MP write a .d file per object listing the headers it includes,
# so editing a header rebuilds every .c file that uses it.
src/%.o: src/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f tinysql $(OBJS) $(DEPS)

-include $(DEPS)

.PHONY: clean
