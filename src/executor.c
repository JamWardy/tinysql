#include "executor.h"

#include <stdio.h>
#include <stdlib.h>
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

// The Value to pass to the table for lit. Its text points into lit, so
// it's only for functions that copy it, and lit must outlive it.
static Value literal_to_value(const Literal *lit) {
    Value value;

    value.type = lit->type;
    if (lit->type == TYPE_INT) {
        value.as.integer = literal_as_int(lit);
    } else {
        // Value.as.text isn't const because the table frees its own
        // copies; the table functions only read this one and copy it.
        value.as.text = (char *)literal_as_text(lit);
    }
    return value;
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

        values[i] = literal_to_value(lit);
    }

    if (!table_append_row(table, values)) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }

    return 1;
}

// A WHERE condition tree checked and resolved against a table.
typedef struct {
    const Condition *conditions;     // The statement's nodes.
    size_t columns[MAX_CONDITIONS];  // Per COND_COMPARE node, its column.
    int root;
} Filter;

// Returns <0, 0 or >0 as a is less than, equal to or greater than b,
// which must have the same type. Text compares byte by byte, so
// 'B' < 'a' and '10' < '9'.
static int compare_values(const Value *a, const Value *b) {
    if (a->type == TYPE_INT) {
        long long x = a->as.integer;
        long long y = b->as.integer;
        return (x > y) - (x < y);
    }

    return strcmp(a->as.text, b->as.text);
}

static int comparison_matches(const Value *value, const Comparison *cmp) {
    Value wanted = literal_to_value(&cmp->value);
    int c = compare_values(value, &wanted);

    switch (cmp->op) {
        case CMP_EQUAL:         return c == 0;
        case CMP_NOT_EQUAL:     return c != 0;
        case CMP_LESS:          return c < 0;
        case CMP_LESS_EQUAL:    return c <= 0;
        case CMP_GREATER:       return c > 0;
        case CMP_GREATER_EQUAL: return c >= 0;
    }
    return 0;
}

// Evaluates the condition node at index against row, recursing into
// its children.
static int condition_matches(const Filter *filter, const Row *row,
                             int index) {
    const Condition *cond = &filter->conditions[index];

    switch (cond->type) {
        case COND_COMPARE:
            return comparison_matches(&row->values[filter->columns[index]],
                                      &cond->as.compare);
        case COND_AND:
            return condition_matches(filter, row, cond->as.children.left) &&
                   condition_matches(filter, row, cond->as.children.right);
        case COND_OR:
            return condition_matches(filter, row, cond->as.children.left) ||
                   condition_matches(filter, row, cond->as.children.right);
        case COND_NOT:
            return !condition_matches(filter, row, cond->as.children.left);
    }
    return 0;
}

// A NULL filter matches every row.
static int row_matches(const Row *row, const Filter *filter) {
    return !filter || condition_matches(filter, row, filter->root);
}

// Checks every comparison in where against table and fills filter.
// Done before any row is read, so a bad column or type is reported
// even when no row would reach that part of the condition.
static int resolve_filter(const Table *table, const WhereClause *where,
                          Filter *filter, char *error, size_t error_size) {
    filter->conditions = where->conditions;
    filter->root = where->root;

    for (int i = 0; i < where->condition_count; i++) {
        if (where->conditions[i].type != COND_COMPARE) {
            continue;
        }

        const Comparison *cmp = &where->conditions[i].as.compare;
        int index = table_find_column(table, cmp->column);

        if (index < 0) {
            snprintf(error, error_size, "no such column '%s'", cmp->column);
            return 0;
        }

        const Column *column = &table->columns[index];
        if (cmp->value.type != column->type) {
            snprintf(error, error_size,
                     "column '%s' is %s but the WHERE value is %s",
                     column->name, data_type_name(column->type),
                     data_type_name(cmp->value.type));
            return 0;
        }

        filter->columns[i] = (size_t)index;
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

// Prints the chosen columns of rows, in order, e.g.
//
//   name  | age
//   ------+----
//   Alice |  25
//   (1 row)
static void print_rows(const Table *table, const size_t *columns,
                       int column_count, const Row *const *rows,
                       size_t row_count) {
    int widths[MAX_COLUMNS];

    for (int i = 0; i < column_count; i++) {
        widths[i] = (int)strlen(table->columns[columns[i]].name);

        for (size_t r = 0; r < row_count; r++) {
            int width = value_width(&rows[r]->values[columns[i]]);
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

    for (size_t r = 0; r < row_count; r++) {
        for (int i = 0; i < column_count; i++) {
            printf("%s", i > 0 ? " | " : "");
            print_value(&rows[r]->values[columns[i]], widths[i]);
        }
        putchar('\n');
    }

    printf("(%zu %s)\n", row_count, row_count == 1 ? "row" : "rows");
}

// qsort passes its comparison function only the two elements, so the
// ORDER BY column and direction are set here just before sorting.
static struct {
    size_t column;
    int descending;
} sort_by;

// Compares two elements of an array of const Row *.
static int compare_rows(const void *a, const void *b) {
    const Row *x = *(const Row *const *)a;
    const Row *y = *(const Row *const *)b;
    int c = compare_values(&x->values[sort_by.column],
                           &y->values[sort_by.column]);

    if (sort_by.descending) {
        c = -c;
    }

    // qsort may reorder equal elements, so break ties by position in
    // the table (the rows are one array), keeping insertion order.
    if (c == 0) {
        c = (x > y) - (x < y);
    }
    return c;
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

    Filter filter;

    if (select->has_where &&
        !resolve_filter(table, &select->where, &filter, error, error_size)) {
        return 0;
    }

    int order_column = -1;

    if (select->has_order) {
        order_column = table_find_column(table, select->order_column);

        if (order_column < 0) {
            snprintf(error, error_size, "no such column '%s'",
                     select->order_column);
            return 0;
        }
    }

    // Collect the matching rows, so they can be sorted and cut to the
    // limit before printing. Pointers, not copies: the rows stay put.
    const Row **rows = malloc(table->row_count * sizeof(*rows));

    // malloc(0) may return NULL, which isn't a failure.
    if (!rows && table->row_count > 0) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }

    const Filter *f = select->has_where ? &filter : NULL;
    size_t count = 0;

    for (size_t r = 0; r < table->row_count; r++) {
        if (row_matches(&table->rows[r], f)) {
            rows[count++] = &table->rows[r];
        }
    }

    // Skipped for under two rows; rows may be NULL, which qsort doesn't
    // allow even with nothing to sort.
    if (select->has_order && count > 1) {
        sort_by.column = (size_t)order_column;
        sort_by.descending = select->descending;
        qsort(rows, count, sizeof(*rows), compare_rows);
    }

    if (select->has_limit && (unsigned long long)select->limit < count) {
        count = (size_t)select->limit;
    }

    print_rows(table, columns, column_count, rows, count);
    free(rows);
    return 1;
}

static int execute_delete(Database *db, const Statement *stmt,
                          char *error, size_t error_size) {
    const DeleteStatement *delete = &stmt->as.delete;
    Table *table = database_find_table(db, stmt->table_name);

    if (!table) {
        snprintf(error, error_size, "no such table '%s'", stmt->table_name);
        return 0;
    }

    // As in SELECT, check the whole WHERE before touching any row, so a
    // bad column can't leave a delete half done.
    Filter filter;

    if (delete->has_where &&
        !resolve_filter(table, &delete->where, &filter, error, error_size)) {
        return 0;
    }

    const Filter *f = delete->has_where ? &filter : NULL;
    size_t deleted = 0;
    size_t r = 0;

    // Only move on when the row is kept: deleting shifts the next row
    // into position r.
    while (r < table->row_count) {
        if (row_matches(&table->rows[r], f)) {
            table_delete_row(table, r);
            deleted++;
        } else {
            r++;
        }
    }

    printf("(%zu %s deleted)\n", deleted, deleted == 1 ? "row" : "rows");
    return 1;
}

static int execute_update(Database *db, const Statement *stmt,
                          char *error, size_t error_size) {
    const UpdateStatement *update = &stmt->as.update;
    Table *table = database_find_table(db, stmt->table_name);

    if (!table) {
        snprintf(error, error_size, "no such table '%s'", stmt->table_name);
        return 0;
    }

    // Check every assignment and the WHERE before changing any row.
    size_t columns[MAX_COLUMNS];
    Value values[MAX_COLUMNS];

    for (int i = 0; i < update->assignment_count; i++) {
        const Assignment *assign = &update->assignments[i];
        int index = table_find_column(table, assign->column);

        if (index < 0) {
            snprintf(error, error_size, "no such column '%s'",
                     assign->column);
            return 0;
        }

        const Column *column = &table->columns[index];
        if (assign->value.type != column->type) {
            snprintf(error, error_size,
                     "column '%s' is %s but the value is %s",
                     column->name, data_type_name(column->type),
                     data_type_name(assign->value.type));
            return 0;
        }

        for (int j = 0; j < i; j++) {
            if (columns[j] == (size_t)index) {
                snprintf(error, error_size, "column '%s' is set twice",
                         column->name);
                return 0;
            }
        }

        columns[i] = (size_t)index;
        values[i] = literal_to_value(&assign->value);
    }

    Filter filter;

    if (update->has_where &&
        !resolve_filter(table, &update->where, &filter, error, error_size)) {
        return 0;
    }

    const Filter *f = update->has_where ? &filter : NULL;
    size_t updated = 0;

    for (size_t r = 0; r < table->row_count; r++) {
        // Each row is tested before any of its values change, and a
        // change to one row can't affect the test on another.
        if (!row_matches(&table->rows[r], f)) {
            continue;
        }

        for (int i = 0; i < update->assignment_count; i++) {
            // Only text needs memory. Running out partway leaves the
            // changes made so far in place; there's no undo.
            if (!table_set_value(table, r, columns[i], &values[i])) {
                snprintf(error, error_size,
                         "out of memory after updating %zu rows", updated);
                return 0;
            }
        }
        updated++;
    }

    printf("(%zu %s updated)\n", updated, updated == 1 ? "row" : "rows");
    return 1;
}

static int execute_drop(Database *db, const Statement *stmt,
                        char *error, size_t error_size) {
    Table *table = database_find_table(db, stmt->table_name);

    if (!table) {
        snprintf(error, error_size, "no such table '%s'", stmt->table_name);
        return 0;
    }

    database_drop_table(db, table);
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
        case STMT_DELETE:
            return execute_delete(db, stmt, error, error_size);
        case STMT_UPDATE:
            return execute_update(db, stmt, error, error_size);
        case STMT_DROP:
            return execute_drop(db, stmt, error, error_size);
    }

    snprintf(error, error_size, "unknown statement type");
    return 0;
}
