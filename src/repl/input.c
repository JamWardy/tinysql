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

// Where reading a statement has got to, carried from line to line so
// a ';' inside a string or after "--" doesn't end the statement.
typedef struct {
    int in_string;   // Inside '...', which can span lines.
    int in_comment;  // After "--", until the end of the line.
    int dash;        // The last character was a '-', maybe starting "--".
    int ends;        // The last character that counts was ';'.
} Scan;

static void scan_char(Scan *scan, char c) {
    if (scan->in_comment) {
        scan->in_comment = c != '\n';
        return;
    }
    if (scan->in_string) {
        scan->in_string = c != '\'';  // A '' reopens it on the next quote.
        return;
    }
    if (scan->dash) {
        scan->dash = 0;
        if (c == '-') {
            scan->in_comment = 1;
            return;
        }
        scan->ends = 0;  // The '-' was a minus sign.
    }
    if (c == '-') {
        scan->dash = 1;
    } else if (c == '\'') {
        scan->in_string = 1;
        scan->ends = 0;
    } else if (!isspace((unsigned char)c)) {
        scan->ends = c == ';';
    }
}

static void scan_text(Scan *scan, const char *text) {
    for (; *text != '\0'; text++) {
        scan_char(scan, *text);
    }
}

// Whether the last character outside strings and comments is ';'.
static int scan_ended(const Scan *scan) {
    return scan->ends && !scan->dash;
}

// Discards the rest of the line, still scanning it.
static void skip_line(Scan *scan) {
    int c;

    while ((c = getchar()) != EOF) {
        scan_char(scan, (char)c);
        if (c == '\n') {
            break;
        }
    }
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

// Without the spaces around it or the newline, which the file's last line may
// lack.
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

static int is_comment(const char *line) {
    return strncmp(skip_spaces(line), "--", 2) == 0;
}

// A line that doesn't fit is still read and scanned to its end, so the next
// read starts on the next line.
static LineResult read_line(char *line, size_t size, Scan *scan) {
    if (fgets(line, (int)size, stdin) == NULL) {
        return LINE_END;
    }

    size_t length = strlen(line);

    scan_text(scan, line);

    // fgets stops early when line is full, leaving the rest waiting.
    if (length == size - 1 && line[length - 1] != '\n') {
        skip_line(scan);
        return LINE_TOO_LONG;
    }
    return LINE_OK;
}

// Changes nothing if it won't fit in size bytes.
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
    Scan scan = {0};

    while (1) {
        int in_statement = length > 0 || too_long;

        printf("%s", in_statement ? "  -> " : "sql> ");
        fflush(stdout);

        LineResult result = read_line(line, sizeof(line), &scan);

        if (result == LINE_END) {
            if (in_statement) {
                puts("\nError: input ended before ';'");
            }
            return READ_END;
        }

        // Only the start of a statement can be a command, blank or only
        // a comment. A comment that didn't fit has had its rest skipped.
        if (!in_statement) {
            if (result == LINE_OK && is_command(line)) {
                copy_command(statement, size, line);
                return READ_COMMAND;
            }
            if ((result == LINE_OK && is_blank(line)) || is_comment(line)) {
                continue;
            }
        }

        // Once the statement is too long, its remaining lines are read
        // only to find where it ends.
        if (!too_long) {
            too_long = result == LINE_TOO_LONG ||
                       !append_line(statement, size, &length, line);
        }

        if (scan_ended(&scan)) {
            if (too_long) {
                printf("Error: statement too long (max %d characters)\n",
                       MAX_STATEMENT - 1);
                return READ_ERROR;
            }
            return READ_STATEMENT;
        }
    }
}
