#include "print.h"

#include <stdio.h>
#include <string.h>

// How a NULL prints. Printing nothing, as some databases do, would look
// the same as the empty string ''.
#define NULL_TEXT "NULL"

// Number of characters the value takes when printed.
static int value_width(const Value *value) {
    switch (value->type) {
        case TYPE_INT:  return snprintf(NULL, 0, "%lld", value->as.integer);
        case TYPE_TEXT: return (int)strlen(value->as.text);
        case TYPE_NULL: return (int)strlen(NULL_TEXT);
    }
    return 0;
}

static void print_value(const Value *value, int width) {
    // Numbers line up on the right, text and NULL on the left.
    switch (value->type) {
        case TYPE_INT:  printf("%*lld", width, value->as.integer); break;
        case TYPE_TEXT: printf("%-*s", width, value->as.text); break;
        case TYPE_NULL: printf("%-*s", width, NULL_TEXT); break;
    }
}

// Prints result's rows under its column names, e.g.
//
//   name  | age
//   ------+----
//   Alice |  25
//   (1 row)
static void print_rows(const Result *result) {
    int widths[MAX_COLUMNS];

    for (size_t c = 0; c < result->column_count; c++) {
        widths[c] = (int)strlen(result->names[c]);

        for (size_t r = 0; r < result->row_count; r++) {
            int width = value_width(result_value(result, r, c));
            if (width > widths[c]) {
                widths[c] = width;
            }
        }
    }

    for (size_t c = 0; c < result->column_count; c++) {
        printf("%s%-*s", c > 0 ? " | " : "", widths[c], result->names[c]);
    }
    putchar('\n');

    for (size_t c = 0; c < result->column_count; c++) {
        printf("%s", c > 0 ? "-+-" : "");
        for (int w = 0; w < widths[c]; w++) {
            putchar('-');
        }
    }
    putchar('\n');

    for (size_t r = 0; r < result->row_count; r++) {
        for (size_t c = 0; c < result->column_count; c++) {
            printf("%s", c > 0 ? " | " : "");
            print_value(result_value(result, r, c), widths[c]);
        }
        putchar('\n');
    }

    printf("(%zu %s)\n", result->row_count,
           result->row_count == 1 ? "row" : "rows");
}

// Prints e.g. "(3 rows deleted)".
static void print_changed(size_t count, const char *how) {
    printf("(%zu %s %s)\n", count, count == 1 ? "row" : "rows", how);
}

void print_result(const Result *result) {
    switch (result->type) {
        case STMT_SELECT:
            print_rows(result);
            break;
        case STMT_DELETE:
            print_changed(result->changed, "deleted");
            break;
        case STMT_UPDATE:
            print_changed(result->changed, "updated");
            break;
        case STMT_CREATE:
        case STMT_INSERT:
        case STMT_DROP:
            break;
    }
}
