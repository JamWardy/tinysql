#include "input.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "text.h"

typedef enum {
    LINE_OK,
    LINE_TOO_LONG,  // Only the start of the line was kept.
    LINE_END        // No more input.
} LineResult;

// Reads and discards the rest of the current input line. Returns its
// last non-space character, or '\0' if it has none.
static int skip_line(void) {
    int c;
    int last = '\0';

    while ((c = getchar()) != '\n' && c != EOF) {
        if (!isspace(c)) {
            last = c;
        }
    }

    return last;
}

static const char *skip_spaces(const char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static int is_command(const char *line) {
    return *skip_spaces(line) == '.';
}

// Copies the command in line to command (size bytes), leaving out the
// spaces around it and the newline (the last line of a file may have
// none).
static void copy_command(char *command, size_t size, const char *line) {
    line = skip_spaces(line);
    size_t length = strlen(line);

    while (length > 0 && isspace((unsigned char)line[length - 1])) {
        length--;
    }
    copy_slice(command, size, line, length);
}

static int is_blank(const char *s) {
    return *skip_spaces(s) == '\0';
}

// Returns 1 if line's last non-space character is ';', which ends the
// statement. This doesn't look inside strings: the tokeniser deals with
// those once the statement is complete.
static int ends_statement(const char *line, size_t length) {
    while (length > 0 && isspace((unsigned char)line[length - 1])) {
        length--;
    }
    return length > 0 && line[length - 1] == ';';
}

// Reads one line into line (size bytes) and sets *complete to whether it
// ends the statement. A line that doesn't fit is still read to its end,
// so the next read starts on the next line.
static LineResult read_line(char *line, size_t size, int *complete) {
    if (fgets(line, (int)size, stdin) == NULL) {
        return LINE_END;
    }

    size_t length = strlen(line);

    // fgets stops early when line is full, leaving the rest waiting.
    if (length == size - 1 && line[length - 1] != '\n') {
        int last = skip_line();
        *complete = last != '\0' ? last == ';'
                                 : ends_statement(line, length);
        return LINE_TOO_LONG;
    }

    *complete = ends_statement(line, length);
    return LINE_OK;
}

// Adds line to the end of statement, which holds *length characters.
// Returns 0, changing nothing, if it won't fit in size bytes.
static int append_line(char *statement, size_t size, size_t *length,
                       const char *line) {
    size_t line_length = strlen(line);

    if (*length + line_length >= size) {
        return 0;
    }

    memcpy(statement + *length, line, line_length + 1);
    *length += line_length;
    return 1;
}

ReadResult read_statement(char *statement, size_t size) {
    char line[MAX_STATEMENT];
    size_t length = 0;  // Of statement so far.
    int too_long = 0;   // 1 once the statement no longer fits.

    while (1) {
        int in_statement = length > 0 || too_long;

        printf("%s", in_statement ? "  -> " : "sql> ");
        fflush(stdout);

        int complete;
        LineResult result = read_line(line, sizeof(line), &complete);

        if (result == LINE_END) {
            if (in_statement) {
                puts("\nError: input ended before ';'");
            }
            return READ_END;
        }

        // Only the start of a statement can be a command or blank.
        if (result == LINE_OK && !in_statement) {
            if (is_command(line)) {
                copy_command(statement, size, line);
                return READ_COMMAND;
            }
            if (is_blank(line)) {
                continue;
            }
        }

        // Once the statement is too long, its remaining lines are read
        // only to find where it ends.
        if (!too_long) {
            too_long = result == LINE_TOO_LONG ||
                       !append_line(statement, size, &length, line);
        }

        if (complete) {
            if (too_long) {
                printf("Error: statement too long (max %d characters)\n",
                       MAX_STATEMENT - 1);
                return READ_ERROR;
            }
            return READ_STATEMENT;
        }
    }
}
