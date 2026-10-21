#include "executor.h"

#include <stdio.h>
#include <string.h>

static const char *data_type_name(DataType type) {
    switch (type) {
        case TYPE_INT:  return "INT";
        case TYPE_TEXT: return "TEXT";
    }
    return "UNKNOWN";
}

static int execute_create(Database *db, const Statement *stmt,
                          char *error, size_t error_size) {
    const CreateStatement *create = &stmt->as.create;

    if (database_find_table(db, stmt->table_name)) {
        snprintf(error, error_size, "table '%s' already exists",
                 stmt->table_name);
        return 0;
    }

    for (int i = 0; i < create->column_count; i++) {
        for (int j = 0; j < i; j++) {
            if (strcmp(create->columns[i].name,
                       create->columns[j].name) == 0) {
                snprintf(error, error_size, "duplicate column '%s'",
                         create->columns[i].name);
                return 0;
            }
        }
    }

    if (!database_create_table(db, stmt->table_name, create->columns,
                               (size_t)create->column_count)) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }

    return 1;
}

static int execute_insert(Database *db, const Statement *stmt,
                          char *error, size_t error_size) {
    const InsertStatement *insert = &stmt->as.insert;
    Table *table = database_find_table(db, stmt->table_name);

    if (!table) {
        snprintf(error, error_size, "no such table '%s'", stmt->table_name);
        return 0;
    }

    if ((size_t)insert->value_count != table->column_count) {
        snprintf(error, error_size,
                 "table '%s' has %zu columns but %d values were given",
                 table->name, table->column_count, insert->value_count);
        return 0;
    }

    Value values[MAX_COLUMNS];

    for (int i = 0; i < insert->value_count; i++) {
        const Literal *lit = &insert->values[i];
        const Column *column = &table->columns[i];

        if (lit->type != column->type) {
            snprintf(error, error_size,
                     "column '%s' is %s but the value is %s",
                     column->name, data_type_name(column->type),
                     data_type_name(lit->type));
            return 0;
        }

        values[i].type = lit->type;
        if (lit->type == TYPE_INT) {
            values[i].as.integer = literal_as_int(lit);
        } else {
            // Value.as.text isn't const because the table frees its own
            // copies; table_append_row only reads this one and copies it.
            values[i].as.text = (char *)literal_as_text(lit);
        }
    }

    if (!table_append_row(table, values)) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }

    return 1;
}

// Number of characters the value takes when printed.
static int value_width(const Value *value) {
    if (value->type == TYPE_INT) {
        return snprintf(NULL, 0, "%lld", value->as.integer);
    }
    return (int)strlen(value->as.text);
}

static void print_value(const Value *value, int width) {
    // Numbers line up on the right, text on the left.
    if (value->type == TYPE_INT) {
        printf("%*lld", width, value->as.integer);
    } else {
        printf("%-*s", width, value->as.text);
    }
}

// Prints the chosen columns of every row, e.g.
//
//   name  | age
//   ------+----
//   Alice |  25
//   (1 row)
static void print_rows(const Table *table, const size_t *columns,
                       int column_count) {
    int widths[MAX_COLUMNS];

    for (int i = 0; i < column_count; i++) {
        widths[i] = (int)strlen(table->columns[columns[i]].name);

        for (size_t r = 0; r < table->row_count; r++) {
            int width = value_width(&table->rows[r].values[columns[i]]);
            if (width > widths[i]) {
                widths[i] = width;
            }
        }
    }

    for (int i = 0; i < column_count; i++) {
        printf("%s%-*s", i > 0 ? " | " : "",
               widths[i], table->columns[columns[i]].name);
    }
    putchar('\n');

    for (int i = 0; i < column_count; i++) {
        printf("%s", i > 0 ? "-+-" : "");
        for (int w = 0; w < widths[i]; w++) {
            putchar('-');
        }
    }
    putchar('\n');

    for (size_t r = 0; r < table->row_count; r++) {
        for (int i = 0; i < column_count; i++) {
            printf("%s", i > 0 ? " | " : "");
            print_value(&table->rows[r].values[columns[i]], widths[i]);
        }
        putchar('\n');
    }

    printf("(%zu %s)\n", table->row_count,
           table->row_count == 1 ? "row" : "rows");
}

static int execute_select(Database *db, const Statement *stmt,
                          char *error, size_t error_size) {
    const SelectStatement *select = &stmt->as.select;
    const Table *table = database_find_table(db, stmt->table_name);

    if (!table) {
        snprintf(error, error_size, "no such table '%s'", stmt->table_name);
        return 0;
    }

    // Resolve every name before printing anything, so a bad column
    // gives an error rather than half a result.
    size_t columns[MAX_COLUMNS];
    int column_count;

    if (select->select_all) {
        column_count = (int)table->column_count;
        for (int i = 0; i < column_count; i++) {
            columns[i] = (size_t)i;
        }
    } else {
        column_count = select->column_count;
        for (int i = 0; i < column_count; i++) {
            int index = table_find_column(table, select->columns[i]);

            if (index < 0) {
                snprintf(error, error_size, "no such column '%s'",
                         select->columns[i]);
                return 0;
            }
            columns[i] = (size_t)index;
        }
    }

    print_rows(table, columns, column_count);
    return 1;
}

int execute(Database *db, const Statement *stmt,
            char *error, size_t error_size) {
    switch (stmt->type) {
        case STMT_CREATE:
            return execute_create(db, stmt, error, error_size);
        case STMT_INSERT:
            return execute_insert(db, stmt, error, error_size);
        case STMT_SELECT:
            return execute_select(db, stmt, error, error_size);
    }

    snprintf(error, error_size, "unknown statement type");
    return 0;
}
