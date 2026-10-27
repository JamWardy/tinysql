#include "executor.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"

// Returns the table called name, or NULL after writing an error.
static Table *find_table(const Database *db, const char *name,
                         char *error, size_t error_size) {
    Table *table = database_find_table(db, name);

    if (!table) {
        fail(error, error_size, "no such table '%s'", name);
    }
    return table;
}

// Checks that a value of type given can go in column. NULL can go in
// any column. what names the value in the error, e.g. "value" or
// "WHERE value".
static int check_type(const Column *column, DataType given, const char *what,
                      char *error, size_t error_size) {
    if (given != TYPE_NULL && given != column->type) {
        return fail(error, error_size, "column '%s' is %s but the %s is %s",
                    column->name, data_type_name(column->type), what,
                    data_type_name(given));
    }
    return 1;
}

// Checks that a value of type given can be stored in column by INSERT
// or UPDATE: it has the right type, and isn't NULL if the column is NOT
// NULL. A WHERE uses check_type instead, since IS NULL is fine on any
// column.
static int check_value(const Column *column, DataType given,
                       char *error, size_t error_size) {
    if (given == TYPE_NULL && column->not_null) {
        return fail(error, error_size, "column '%s' can't be NULL",
                    column->name);
    }
    return check_type(column, given, "value", error, error_size);
}

static int execute_create(Database *db, const Statement *stmt,
                          char *error, size_t error_size) {
    const CreateStatement *create = &stmt->as.create;

    if (database_find_table(db, stmt->table_name)) {
        return fail(error, error_size, "table '%s' already exists",
                    stmt->table_name);
    }

    for (int i = 0; i < create->column_count; i++) {
        for (int j = 0; j < i; j++) {
            if (strcmp(create->columns[i].name,
                       create->columns[j].name) == 0) {
                return fail(error, error_size, "duplicate column '%s'",
                            create->columns[i].name);
            }
        }
    }

    if (!database_create_table(db, stmt->table_name, create->columns,
                               (size_t)create->column_count)) {
        return fail(error, error_size, "out of memory");
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
    } else if (lit->type == TYPE_TEXT) {
        // Value.as.text isn't const because the table frees its own
        // copies; the table functions only read this one and copy it.
        value.as.text = (char *)literal_as_text(lit);
    }
    return value;
}

static int execute_insert(Database *db, const Statement *stmt,
                          char *error, size_t error_size) {
    const InsertStatement *insert = &stmt->as.insert;
    Table *table = find_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    if ((size_t)insert->value_count != table->column_count) {
        return fail(error, error_size,
                    "table '%s' has %zu columns but %d values were given",
                    table->name, table->column_count, insert->value_count);
    }

    Value values[MAX_COLUMNS];

    for (int i = 0; i < insert->value_count; i++) {
        const Literal *lit = &insert->values[i];

        if (!check_value(&table->columns[i], lit->type, error, error_size)) {
            return 0;
        }

        values[i] = literal_to_value(lit);
    }

    if (!table_append_row(table, values)) {
        return fail(error, error_size, "out of memory");
    }

    return 1;
}

// A WHERE condition tree checked and resolved against a table.
typedef struct {
    const Condition *conditions;     // The statement's nodes.
    size_t columns[MAX_CONDITIONS];  // Per COND_COMPARE node, its column.
    int root;
} Filter;

// Table columns a query reads, by position in each row: the selected
// columns, or the column each aggregate reads.
typedef struct {
    size_t index[MAX_COLUMNS];
    size_t count;
} ColumnList;

// The rows a query matched. These are pointers into the table, not
// copies, so they're valid only until the table next changes. Whoever
// fills one frees rows when done.
typedef struct {
    const Row **rows;
    size_t count;
} RowList;

// Returns <0, 0 or >0 as a is less than, equal to or greater than b,
// which must have the same type and not be NULL. Text compares byte by
// byte, so 'B' < 'a' and '10' < '9'.
static int compare_values(const Value *a, const Value *b) {
    if (a->type == TYPE_INT) {
        long long x = a->as.integer;
        long long y = b->as.integer;
        return (x > y) - (x < y);
    }

    return strcmp(a->as.text, b->as.text);
}

// SQL's three-valued logic. A comparison with NULL, such as `age > 30`
// when age is NULL, is neither true nor false but unknown: NULL means
// "no value", so there's nothing to compare. WHERE keeps a row only
// when its condition is true, so unknown rows are left out.
typedef enum {
    TRUTH_FALSE,
    TRUTH_TRUE,
    TRUTH_UNKNOWN
} Truth;

static Truth truth(int b) {
    return b ? TRUTH_TRUE : TRUTH_FALSE;
}

static Truth comparison_truth(const Value *value, const Comparison *cmp) {
    if (cmp->op == CMP_IS_NULL) {
        return truth(value->type == TYPE_NULL);
    }
    if (cmp->op == CMP_IS_NOT_NULL) {
        return truth(value->type != TYPE_NULL);
    }
    if (value->type == TYPE_NULL) {
        return TRUTH_UNKNOWN;
    }

    Value wanted = literal_to_value(&cmp->value);
    int c = compare_values(value, &wanted);

    switch (cmp->op) {
        case CMP_EQUAL:         return truth(c == 0);
        case CMP_NOT_EQUAL:     return truth(c != 0);
        case CMP_LESS:          return truth(c < 0);
        case CMP_LESS_EQUAL:    return truth(c <= 0);
        case CMP_GREATER:       return truth(c > 0);
        case CMP_GREATER_EQUAL: return truth(c >= 0);
        case CMP_IS_NULL:
        case CMP_IS_NOT_NULL:
            break;  // Handled above.
    }
    return TRUTH_FALSE;
}

// Evaluates the condition node at index against row, recursing into
// its children. Unknown spreads only where it could change the answer:
// false AND unknown is false, and true OR unknown is true, whatever the
// unknown value turns out to be.
static Truth condition_truth(const Filter *filter, const Row *row,
                             int index) {
    const Condition *cond = &filter->conditions[index];

    switch (cond->type) {
        case COND_COMPARE:
            return comparison_truth(&row->values[filter->columns[index]],
                                    &cond->as.compare);
        case COND_AND: {
            Truth left = condition_truth(filter, row, cond->as.children.left);
            if (left == TRUTH_FALSE) {
                return TRUTH_FALSE;
            }
            Truth right = condition_truth(filter, row,
                                          cond->as.children.right);
            if (right == TRUTH_FALSE) {
                return TRUTH_FALSE;
            }
            return left == TRUTH_TRUE && right == TRUTH_TRUE
                       ? TRUTH_TRUE : TRUTH_UNKNOWN;
        }
        case COND_OR: {
            Truth left = condition_truth(filter, row, cond->as.children.left);
            if (left == TRUTH_TRUE) {
                return TRUTH_TRUE;
            }
            Truth right = condition_truth(filter, row,
                                          cond->as.children.right);
            if (right == TRUTH_TRUE) {
                return TRUTH_TRUE;
            }
            return left == TRUTH_FALSE && right == TRUTH_FALSE
                       ? TRUTH_FALSE : TRUTH_UNKNOWN;
        }
        case COND_NOT:
            switch (condition_truth(filter, row, cond->as.children.left)) {
                case TRUTH_FALSE:   return TRUTH_TRUE;
                case TRUTH_TRUE:    return TRUTH_FALSE;
                case TRUTH_UNKNOWN: return TRUTH_UNKNOWN;
            }
    }
    return TRUTH_FALSE;
}

// A NULL filter matches every row.
static int row_matches(const Row *row, const Filter *filter) {
    return !filter ||
           condition_truth(filter, row, filter->root) == TRUTH_TRUE;
}

// Sets *index to the position of the column called name in table, the
// position of its value in each row. Every name the SQL uses is looked
// up this way once, before any row is read.
static int find_column(const Table *table, const char *name,
                       size_t *index, char *error, size_t error_size) {
    if (!table_find_column(table, name, index)) {
        return fail(error, error_size, "no such column '%s'", name);
    }
    return 1;
}

// Checks every comparison in where against table and fills filter, then
// sets *out to filter, or to NULL (matching every row) when has_where
// is 0. Done before any row is read, so a bad column or type is
// reported even when no row would reach that part of the condition.
static int resolve_where(const Table *table, int has_where,
                         const WhereClause *where, Filter *filter,
                         const Filter **out, char *error, size_t error_size) {
    *out = NULL;

    if (!has_where) {
        return 1;
    }

    filter->conditions = where->conditions;
    filter->root = where->root;

    for (int i = 0; i < where->condition_count; i++) {
        if (where->conditions[i].type != COND_COMPARE) {
            continue;
        }

        const Comparison *cmp = &where->conditions[i].as.compare;
        size_t index;

        if (!find_column(table, cmp->column, &index, error, error_size)) {
            return 0;
        }

        if (!check_type(&table->columns[index], cmp->value.type,
                        "WHERE value", error, error_size)) {
            return 0;
        }

        filter->columns[i] = index;
    }

    *out = filter;
    return 1;
}

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

// Prints the chosen columns of rows, in order, under the headings in
// names (one per chosen column), e.g.
//
//   name  | age
//   ------+----
//   Alice |  25
//   (1 row)
static void print_rows(const char *const *names, const ColumnList *columns,
                       const RowList *rows) {
    int widths[MAX_COLUMNS];

    for (size_t i = 0; i < columns->count; i++) {
        widths[i] = (int)strlen(names[i]);

        for (size_t r = 0; r < rows->count; r++) {
            int width = value_width(&rows->rows[r]->values[columns->index[i]]);
            if (width > widths[i]) {
                widths[i] = width;
            }
        }
    }

    for (size_t i = 0; i < columns->count; i++) {
        printf("%s%-*s", i > 0 ? " | " : "", widths[i], names[i]);
    }
    putchar('\n');

    for (size_t i = 0; i < columns->count; i++) {
        printf("%s", i > 0 ? "-+-" : "");
        for (int w = 0; w < widths[i]; w++) {
            putchar('-');
        }
    }
    putchar('\n');

    for (size_t r = 0; r < rows->count; r++) {
        for (size_t i = 0; i < columns->count; i++) {
            printf("%s", i > 0 ? " | " : "");
            print_value(&rows->rows[r]->values[columns->index[i]], widths[i]);
        }
        putchar('\n');
    }

    printf("(%zu %s)\n", rows->count, rows->count == 1 ? "row" : "rows");
}

// qsort passes its comparison function only the two elements, so the
// ORDER BY column and direction are set here just before sorting.
static struct {
    size_t column;
    int descending;
} sort_by;

// compare_values for sorting, where either value may be NULL. NULL
// sorts after every other value, as in PostgreSQL, so NULLs come last
// in ascending order and first in descending order.
static int compare_for_sort(const Value *a, const Value *b) {
    int a_null = a->type == TYPE_NULL;
    int b_null = b->type == TYPE_NULL;

    if (a_null || b_null) {
        return a_null - b_null;
    }
    return compare_values(a, b);
}

// Compares two elements of an array of const Row *.
static int compare_rows(const void *a, const void *b) {
    const Row *x = *(const Row *const *)a;
    const Row *y = *(const Row *const *)b;
    int c = compare_for_sort(&x->values[sort_by.column],
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

static const char *aggregate_name(AggregateFunc func) {
    switch (func) {
        case AGG_COUNT: return "COUNT";
        case AGG_SUM:   return "SUM";
        case AGG_MIN:   return "MIN";
        case AGG_MAX:   return "MAX";
    }
    return "UNKNOWN";
}

// Writes the column heading for agg into heading, e.g. "COUNT(*)" or
// "MAX(age)". Only formats the text; print_rows prints it.
static void aggregate_heading(const Aggregate *agg, char *heading,
                              size_t size) {
    snprintf(heading, size, "%s(%s)", aggregate_name(agg->func),
             agg->column[0] == '\0' ? "*" : agg->column);
}

// Counts rows for COUNT(*), or for COUNT(column) the rows whose value
// at index column isn't NULL.
static long long count_values(const Aggregate *agg, size_t column,
                              const RowList *rows) {
    if (agg->column[0] == '\0') {
        return (long long)rows->count;
    }

    long long count = 0;
    for (size_t r = 0; r < rows->count; r++) {
        if (rows->rows[r]->values[column].type != TYPE_NULL) {
            count++;
        }
    }
    return count;
}

// Sets *out to agg over rows, reading the table column at index column
// (unused for COUNT(*)). Text in *out points into a row, so it's only
// for printing straight away.
//
// COUNT(*) counts rows, NULLs and all, and COUNT(column) the non-NULL
// values; both give 0 when there are none. SUM, MIN and MAX skip NULLs,
// and give NULL when there's no other value: no rows, or only NULLs.
static int compute_aggregate(const Aggregate *agg, size_t column,
                             const RowList *rows, Value *out,
                             char *error, size_t error_size) {
    if (agg->func == AGG_COUNT) {
        out->type = TYPE_INT;
        out->as.integer = count_values(agg, column, rows);
        return 1;
    }

    out->type = TYPE_NULL;

    for (size_t r = 0; r < rows->count; r++) {
        const Value *value = &rows->rows[r]->values[column];

        if (value->type == TYPE_NULL) {
            continue;
        }

        // The first value found starts the result.
        if (out->type == TYPE_NULL) {
            *out = *value;
            continue;
        }

        switch (agg->func) {
            case AGG_SUM: {
                long long x = value->as.integer;
                long long sum = out->as.integer;

                // Check before adding: in C, overflowing a signed
                // integer is undefined behaviour, not a wrap-around.
                if ((x > 0 && sum > LLONG_MAX - x) ||
                    (x < 0 && sum < LLONG_MIN - x)) {
                    return fail(error, error_size, "SUM(%s) is out of range",
                                agg->column);
                }
                out->as.integer = sum + x;
                break;
            }
            case AGG_MIN:
                if (compare_values(value, out) < 0) {
                    *out = *value;
                }
                break;
            case AGG_MAX:
                if (compare_values(value, out) > 0) {
                    *out = *value;
                }
                break;
            case AGG_COUNT:
                break;  // Handled above.
        }
    }

    return 1;
}

// Prints select's aggregates over rows as a single result row, e.g.
//
//   COUNT(*) | MAX(age)
//   ---------+---------
//          3 |       41
//   (1 row)
//
// columns holds the table column each aggregate reads, as filled by
// resolve_aggregates. There's only one result row, so ORDER BY has
// nothing to sort and LIMIT matters only when it's 0: then the headings
// print but the row doesn't.
static int print_aggregates(const SelectStatement *select,
                            const ColumnList *columns, const RowList *rows,
                            char *error, size_t error_size) {
    // Room for the longest function name, the column name, "(", ")".
    char headings[MAX_COLUMNS][MAX_TOKEN_LENGTH + 8];
    const char *names[MAX_COLUMNS];
    Value values[MAX_COLUMNS];
    ColumnList positions;  // Where each result is in values.

    // Compute every result before printing, so an error such as SUM
    // overflowing doesn't leave half a table.
    for (size_t i = 0; i < columns->count; i++) {
        const Aggregate *agg = &select->aggregates[i];

        if (!compute_aggregate(agg, columns->index[i], rows, &values[i],
                               error, error_size)) {
            return 0;
        }

        aggregate_heading(agg, headings[i], sizeof(headings[i]));
        names[i] = headings[i];
        positions.index[i] = i;
    }
    positions.count = columns->count;

    // The results make one row of their own, so print_rows can lay it
    // out like any other.
    Row result = { values, columns->count };
    const Row *result_rows[] = { &result };
    int show_row = !select->has_limit || select->limit > 0;
    RowList result_list = { result_rows, show_row ? 1 : 0 };

    print_rows(names, &positions, &result_list);
    return 1;
}

// Fills out with the table column for each selected column: all of
// them, in order, for SELECT *.
static int resolve_columns(const Table *table, const SelectStatement *select,
                           ColumnList *out, char *error, size_t error_size) {
    if (select->select_all) {
        out->count = table->column_count;
        for (size_t i = 0; i < out->count; i++) {
            out->index[i] = i;
        }
        return 1;
    }

    out->count = (size_t)select->column_count;
    for (size_t i = 0; i < out->count; i++) {
        if (!find_column(table, select->columns[i], &out->index[i],
                         error, error_size)) {
            return 0;
        }
    }

    return 1;
}

// Fills out with the table column each aggregate reads, checking that
// SUM is given an INT column.
static int resolve_aggregates(const Table *table,
                              const SelectStatement *select,
                              ColumnList *out, char *error, size_t error_size) {
    out->count = (size_t)select->aggregate_count;

    for (size_t i = 0; i < out->count; i++) {
        const Aggregate *agg = &select->aggregates[i];

        if (agg->column[0] == '\0') {
            out->index[i] = 0;  // COUNT(*) reads no column; 0 is a placeholder
            continue;
        }

        if (!find_column(table, agg->column, &out->index[i],
                         error, error_size)) {
            return 0;
        }

        const Column *column = &table->columns[out->index[i]];
        if (agg->func == AGG_SUM && column->type != TYPE_INT) {
            return fail(error, error_size,
                        "SUM needs an INT column but '%s' is %s",
                        column->name, data_type_name(column->type));
        }
    }

    return 1;
}

// Sets *order_column to the ORDER BY column. With no ORDER BY it's set
// to 0 and unused: select->has_order says whether there is one.
static int resolve_order_by(const Table *table,
                            const SelectStatement *select,
                            size_t *order_column,
                            char *error, size_t error_size) {
    *order_column = 0;

    if (!select->has_order) {
        return 1;
    }

    return find_column(table, select->order_column, order_column,
                       error, error_size);
}

// Fills out with the rows filter matches, in table order. out->rows is
// a new array, which the caller frees.
static int collect_rows(const Table *table, const Filter *filter,
                        RowList *out, char *error, size_t error_size) {
    out->rows = malloc(table->row_count * sizeof(*out->rows));

    // malloc(0) may return NULL, which isn't a failure.
    if (!out->rows && table->row_count > 0) {
        return fail(error, error_size, "out of memory");
    }

    out->count = 0;
    for (size_t r = 0; r < table->row_count; r++) {
        if (row_matches(&table->rows[r], filter)) {
            out->rows[out->count++] = &table->rows[r];
        }
    }

    return 1;
}

// Sorts rows in place by the ORDER BY column, if there is one, cuts
// rows->count to the LIMIT and prints the chosen columns.
static void print_sorted_rows(const Table *table,
                              const SelectStatement *select,
                              const ColumnList *columns, size_t order_column,
                              RowList *rows) {
    // Skipped for under two rows; rows->rows may be NULL, which qsort
    // doesn't allow even with nothing to sort.
    if (select->has_order && rows->count > 1) {
        sort_by.column = order_column;
        sort_by.descending = select->descending;
        qsort(rows->rows, rows->count, sizeof(*rows->rows), compare_rows);
    }

    if (select->has_limit &&
        (unsigned long long)select->limit < rows->count) {
        rows->count = (size_t)select->limit;
    }

    const char *names[MAX_COLUMNS];

    for (size_t i = 0; i < columns->count; i++) {
        names[i] = table->columns[columns->index[i]].name;
    }

    print_rows(names, columns, rows);
}

// SELECT of columns, e.g. SELECT name, age FROM people WHERE age > 30.
static int select_rows(const Table *table, const SelectStatement *select,
                       char *error, size_t error_size) {
    // Check every name before reading any row, so a bad query fails
    // before any work is done or anything is printed.
    ColumnList columns;
    Filter filter;
    const Filter *where;
    size_t order_column;

    if (!resolve_columns(table, select, &columns, error, error_size) ||
        !resolve_where(table, select->has_where, &select->where, &filter,
                       &where, error, error_size) ||
        !resolve_order_by(table, select, &order_column,
                          error, error_size)) {
        return 0;
    }

    RowList matched;

    if (!collect_rows(table, where, &matched, error, error_size)) {
        return 0;
    }

    print_sorted_rows(table, select, &columns, order_column, &matched);
    free(matched.rows);
    return 1;
}

// SELECT of aggregates, e.g. SELECT COUNT(*), MAX(age) FROM people.
static int select_aggregates(const Table *table,
                             const SelectStatement *select,
                             char *error, size_t error_size) {
    ColumnList columns;  // The column each aggregate reads.
    Filter filter;
    const Filter *where;
    size_t order_column;  // Unused: one result row has nothing to sort.

    // ORDER BY is still resolved, so a bad column name is an error here
    // as it is everywhere else.
    if (!resolve_aggregates(table, select, &columns, error, error_size) ||
        !resolve_where(table, select->has_where, &select->where, &filter,
                       &where, error, error_size) ||
        !resolve_order_by(table, select, &order_column,
                          error, error_size)) {
        return 0;
    }

    RowList matched;

    if (!collect_rows(table, where, &matched, error, error_size)) {
        return 0;
    }

    int ok = print_aggregates(select, &columns, &matched, error, error_size);
    free(matched.rows);
    return ok;
}

static int execute_select(Database *db, const Statement *stmt,
                          char *error, size_t error_size) {
    const SelectStatement *select = &stmt->as.select;
    const Table *table = find_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    return select->has_aggregates
               ? select_aggregates(table, select, error, error_size)
               : select_rows(table, select, error, error_size);
}

static int execute_delete(Database *db, const Statement *stmt,
                          char *error, size_t error_size) {
    const DeleteStatement *delete = &stmt->as.delete;
    Table *table = find_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    // As in SELECT, check the whole WHERE before touching any row, so a
    // bad column can't leave a delete half done.
    Filter filter;
    const Filter *where;

    if (!resolve_where(table, delete->has_where, &delete->where, &filter,
                       &where, error, error_size)) {
        return 0;
    }

    size_t deleted = 0;
    size_t r = 0;

    // Only move on when the row is kept: deleting shifts the next row
    // into position r.
    while (r < table->row_count) {
        if (row_matches(&table->rows[r], where)) {
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
    Table *table = find_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    // Check every assignment and the WHERE before changing any row.
    size_t columns[MAX_COLUMNS];
    Value values[MAX_COLUMNS];

    for (int i = 0; i < update->assignment_count; i++) {
        const Assignment *assign = &update->assignments[i];
        size_t index;

        if (!find_column(table, assign->column, &index, error, error_size)) {
            return 0;
        }

        if (!check_value(&table->columns[index], assign->value.type,
                         error, error_size)) {
            return 0;
        }

        for (int j = 0; j < i; j++) {
            if (columns[j] == index) {
                return fail(error, error_size, "column '%s' is set twice",
                            assign->column);
            }
        }

        columns[i] = index;
        values[i] = literal_to_value(&assign->value);
    }

    Filter filter;
    const Filter *where;

    if (!resolve_where(table, update->has_where, &update->where, &filter,
                       &where, error, error_size)) {
        return 0;
    }

    size_t updated = 0;

    for (size_t r = 0; r < table->row_count; r++) {
        // Each row is tested before any of its values change, and a
        // change to one row can't affect the test on another.
        if (!row_matches(&table->rows[r], where)) {
            continue;
        }

        for (int i = 0; i < update->assignment_count; i++) {
            // Only text needs memory. Running out partway leaves the
            // changes made so far in place; there's no undo.
            if (!table_set_value(table, r, columns[i], &values[i])) {
                return fail(error, error_size,
                            "out of memory after updating %zu rows", updated);
            }
        }
        updated++;
    }

    printf("(%zu %s updated)\n", updated, updated == 1 ? "row" : "rows");
    return 1;
}

static int execute_drop(Database *db, const Statement *stmt,
                        char *error, size_t error_size) {
    Table *table = find_table(db, stmt->table_name, error, error_size);

    if (!table) {
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

    return fail(error, error_size, "unknown statement type");
}
