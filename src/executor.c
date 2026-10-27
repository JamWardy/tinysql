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

// Fills out with the position of each PRIMARY KEY column, checking that
// each is one of the new table's columns, and only named once.
static int resolve_primary_key(const CreateStatement *create, ColumnList *out,
                               char *error, size_t error_size) {
    out->count = (size_t)create->primary_key_count;

    for (size_t i = 0; i < out->count; i++) {
        const char *name = create->primary_key[i];
        int found = 0;

        for (int c = 0; c < create->column_count && !found; c++) {
            if (strcmp(create->columns[c].name, name) == 0) {
                out->index[i] = (size_t)c;
                found = 1;
            }
        }
        if (!found) {
            return fail(error, error_size,
                        "no such column '%s' in PRIMARY KEY", name);
        }

        for (size_t j = 0; j < i; j++) {
            if (out->index[j] == out->index[i]) {
                return fail(error, error_size,
                            "column '%s' is in the PRIMARY KEY twice", name);
            }
        }
    }

    return 1;
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

    ColumnList primary_key;

    if (create->has_primary_key &&
        !resolve_primary_key(create, &primary_key, error, error_size)) {
        return 0;
    }

    Table *table = database_create_table(db, stmt->table_name,
                                         create->columns,
                                         (size_t)create->column_count);
    if (!table) {
        return fail(error, error_size, "out of memory");
    }

    // Undo the half-made table, so a failure leaves no trace.
    if (create->has_primary_key &&
        !table_add_constraint(table, CONSTRAINT_PRIMARY_KEY, &primary_key)) {
        database_drop_table(db, table);
        return fail(error, error_size, "out of memory");
    }

    return 1;
}

static const char *constraint_name(ConstraintKind kind) {
    switch (kind) {
        case CONSTRAINT_PRIMARY_KEY: return "primary key";
    }
    return "key";
}

// Fails with the message for a second row with values' key, e.g.
// "duplicate primary key in table 'users': id = 5". values is laid out
// like a row.
static int fail_duplicate(const Table *table, const Constraint *constraint,
                          const Value *values, char *error,
                          size_t error_size) {
    const ColumnList *key = &constraint->columns;
    char text[256];  // Cut short if it's long; it's only for the message.
    size_t length = 0;

    text[0] = '\0';
    for (size_t i = 0; i < key->count && length < sizeof(text); i++) {
        const Column *column = &table->columns[key->index[i]];
        const Value *value = &values[key->index[i]];
        const char *separator = i > 0 ? ", " : "";
        int written;

        if (value->type == TYPE_INT) {
            written = snprintf(text + length, sizeof(text) - length,
                               "%s%s = %lld", separator, column->name,
                               value->as.integer);
        } else {
            written = snprintf(text + length, sizeof(text) - length,
                               "%s%s = '%s'", separator, column->name,
                               value->as.text);
        }
        // snprintf returns the length it wanted, which may not have fit;
        // then length passes the end and the loop stops.
        length += written > 0 ? (size_t)written : 0;
    }

    return fail(error, error_size, "duplicate %s in table '%s': %s",
                constraint_name(constraint->kind), table->name, text);
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

    // Found through each key's index, so this doesn't read every row.
    for (size_t i = 0; i < table->constraint_count; i++) {
        const Constraint *constraint = &table->constraints[i];
        size_t other;

        if (table_find_key(table, constraint, values, &other)) {
            return fail_duplicate(table, constraint, values,
                                  error, error_size);
        }
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

// Positions of rows in a table, in ascending order. Valid only until
// the table next changes. Whoever fills one frees positions when done.
typedef struct {
    size_t *positions;
    size_t count;
} PositionList;

// Sets equal_to[c] to the value of each `c = value` that the condition
// at node needs to be true: node itself, or a comparison reached from it
// through ANDs alone. Under an OR or NOT, no single comparison has to
// hold, so those are skipped.
static void find_required_equalities(const Filter *filter, int node,
                                     const Literal **equal_to) {
    const Condition *cond = &filter->conditions[node];

    switch (cond->type) {
        case COND_COMPARE:
            if (cond->as.compare.op == CMP_EQUAL) {
                equal_to[filter->columns[node]] = &cond->as.compare.value;
            }
            break;
        case COND_AND:
            find_required_equalities(filter, cond->as.children.left,
                                     equal_to);
            find_required_equalities(filter, cond->as.children.right,
                                     equal_to);
            break;
        case COND_OR:
        case COND_NOT:
            break;
    }
}

// The key a WHERE pins down: see find_key_lookup.
typedef struct {
    const Constraint *constraint;
    Value values[MAX_COLUMNS];  // Laid out like a row; only the key's set.
} KeyLookup;

// If filter needs `column = value` for every column of one of table's
// keys, as `WHERE id = 5` or `WHERE id = 5 AND age > 30` do for the key
// (id), only the row with that key can match, and the key's index finds
// it without reading every row. Fills out and returns 1 if so.
static int find_key_lookup(const Table *table, const Filter *filter,
                           KeyLookup *out) {
    const Literal *equal_to[MAX_COLUMNS] = { NULL };

    find_required_equalities(filter, filter->root, equal_to);

    for (size_t c = 0; c < table->constraint_count; c++) {
        const ColumnList *key = &table->constraints[c].columns;
        size_t i = 0;

        while (i < key->count && equal_to[key->index[i]]) {
            size_t column = key->index[i];
            out->values[column] = literal_to_value(equal_to[column]);
            i++;
        }

        if (i == key->count) {
            out->constraint = &table->constraints[c];
            return 1;
        }
    }

    return 0;
}

// Fills out with the positions of the rows filter matches, in table
// order, through a key's index when the filter allows it.
static int collect_positions(const Table *table, const Filter *filter,
                             PositionList *out,
                             char *error, size_t error_size) {
    KeyLookup lookup;
    int by_key = filter && find_key_lookup(table, filter, &lookup);
    size_t most = by_key ? 1 : table->row_count;

    out->positions = malloc(most * sizeof(*out->positions));
    out->count = 0;

    // malloc(0) may return NULL, which isn't a failure.
    if (!out->positions && most > 0) {
        return fail(error, error_size, "out of memory");
    }

    if (by_key) {
        size_t r;

        // The rest of the WHERE must hold too, as `age > 30` in
        // `WHERE id = 5 AND age > 30`.
        if (table_find_key(table, lookup.constraint, lookup.values, &r) &&
            row_matches(&table->rows[r], filter)) {
            out->positions[out->count++] = r;
        }
        return 1;
    }

    for (size_t r = 0; r < table->row_count; r++) {
        if (row_matches(&table->rows[r], filter)) {
            out->positions[out->count++] = r;
        }
    }

    return 1;
}

// Fills out with the rows filter matches, in table order. out->rows is
// a new array, which the caller frees.
static int collect_rows(const Table *table, const Filter *filter,
                        RowList *out, char *error, size_t error_size) {
    PositionList matched;

    if (!collect_positions(table, filter, &matched, error, error_size)) {
        return 0;
    }

    out->rows = malloc(matched.count * sizeof(*out->rows));
    if (!out->rows && matched.count > 0) {
        free(matched.positions);
        return fail(error, error_size, "out of memory");
    }

    for (size_t i = 0; i < matched.count; i++) {
        out->rows[i] = &table->rows[matched.positions[i]];
    }
    out->count = matched.count;

    free(matched.positions);
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

    PositionList matched;

    if (!collect_positions(table, where, &matched, error, error_size)) {
        return 0;
    }

    // From the last, so deleting a row doesn't move the ones still to go.
    for (size_t i = matched.count; i > 0; i--) {
        table_delete_row(table, matched.positions[i - 1]);
    }

    printf("(%zu %s deleted)\n", matched.count,
           matched.count == 1 ? "row" : "rows");
    free(matched.positions);
    return 1;
}

// The new value UPDATE gives each column it sets: values[i] goes in
// column columns.index[i].
typedef struct {
    ColumnList columns;
    Value values[MAX_COLUMNS];
} Changes;

// Fills after with row r's values as they'll be after the UPDATE: with
// changes made if updating[r] is set, otherwise as they are. Text points
// into the row or the statement, so after is only for reading straight
// away.
static void values_after_update(const Table *table, size_t r,
                                const unsigned char *updating,
                                const Changes *changes, Value *after) {
    const Row *row = &table->rows[r];

    memcpy(after, row->values, row->count * sizeof(*after));

    if (updating[r]) {
        for (size_t i = 0; i < changes->columns.count; i++) {
            after[changes->columns.index[i]] = changes->values[i];
        }
    }
}

// What check_key_after_update is looking for, passed through index_find.
typedef struct {
    const Table *table;
    const unsigned char *updating;
    const Changes *changes;
    const ColumnList *key;
    const Value *values;  // The key being looked for, laid out like a row.
} UpdatedKeySearch;

static int updated_row_has_key(size_t row, const void *context) {
    const UpdatedKeySearch *search = context;
    Value after[MAX_COLUMNS];

    values_after_update(search->table, row, search->updating,
                        search->changes, after);
    return key_equal(search->key, after, search->values);
}

// Checks that constraint's key would still be unique after the UPDATE,
// by putting every row's new key in a new index, one by one, and failing
// at the first that's there already. This reads every row, but only
// runs when an UPDATE sets a key column, which is rare.
//
// Checking only the updated rows against the table's index isn't
// enough: two updated rows can clash with each other.
static int check_key_after_update(const Table *table,
                                  const Constraint *constraint,
                                  const unsigned char *updating,
                                  const Changes *changes,
                                  char *error, size_t error_size) {
    Index seen;

    index_init(&seen);
    if (!index_reserve(&seen, table->row_count)) {
        return fail(error, error_size, "out of memory");
    }

    int ok = 1;

    for (size_t r = 0; r < table->row_count && ok; r++) {
        Value after[MAX_COLUMNS];
        values_after_update(table, r, updating, changes, after);

        uint64_t hash = key_hash(&constraint->columns, after);
        UpdatedKeySearch search = { table, updating, changes,
                                    &constraint->columns, after };
        size_t other;

        if (index_find(&seen, hash, updated_row_has_key, &search, &other)) {
            ok = fail_duplicate(table, constraint, after, error, error_size);
        } else {
            index_insert(&seen, hash, r);
        }
    }

    index_free(&seen);
    return ok;
}

// Checks that every key would still be unique after the UPDATE changes
// the rows in matched.
static int check_keys_after_update(const Table *table,
                                   const PositionList *matched,
                                   const Changes *changes,
                                   char *error, size_t error_size) {
    unsigned char *updating = NULL;  // Per row, 1 if it's being updated.
    int ok = 1;

    for (size_t c = 0; c < table->constraint_count && ok; c++) {
        const Constraint *constraint = &table->constraints[c];
        int sets_key = 0;

        for (size_t i = 0; i < changes->columns.count; i++) {
            if (column_list_has(&constraint->columns,
                                changes->columns.index[i])) {
                sets_key = 1;
            }
        }

        // A key the UPDATE doesn't set, or sets in no rows, stays
        // unique.
        if (!sets_key || matched->count == 0) {
            continue;
        }

        if (!updating) {
            updating = calloc(table->row_count, 1);
            if (!updating) {
                return fail(error, error_size, "out of memory");
            }
            for (size_t i = 0; i < matched->count; i++) {
                updating[matched->positions[i]] = 1;
            }
        }

        ok = check_key_after_update(table, constraint, updating, changes,
                                    error, error_size);
    }

    free(updating);
    return ok;
}

// Fills out with the column and new value of each of update's
// assignments, checking each column exists, is set at most once, and can
// take its value.
static int resolve_changes(const Table *table, const UpdateStatement *update,
                           Changes *out, char *error, size_t error_size) {
    out->columns.count = (size_t)update->assignment_count;

    for (size_t i = 0; i < out->columns.count; i++) {
        const Assignment *assign = &update->assignments[i];
        size_t index;

        if (!find_column(table, assign->column, &index, error, error_size)) {
            return 0;
        }

        if (!check_value(&table->columns[index], assign->value.type,
                         error, error_size)) {
            return 0;
        }

        for (size_t j = 0; j < i; j++) {
            if (out->columns.index[j] == index) {
                return fail(error, error_size, "column '%s' is set twice",
                            assign->column);
            }
        }

        out->columns.index[i] = index;
        out->values[i] = literal_to_value(&assign->value);
    }

    return 1;
}

static int execute_update(Database *db, const Statement *stmt,
                          char *error, size_t error_size) {
    const UpdateStatement *update = &stmt->as.update;
    Table *table = find_table(db, stmt->table_name, error, error_size);

    if (!table) {
        return 0;
    }

    // Check every assignment, the WHERE and the keys before changing any
    // row.
    Changes changes;
    Filter filter;
    const Filter *where;

    if (!resolve_changes(table, update, &changes, error, error_size) ||
        !resolve_where(table, update->has_where, &update->where, &filter,
                       &where, error, error_size)) {
        return 0;
    }

    // Every row is tested before any changes, so a change to one row
    // can't affect the test on another.
    PositionList matched;

    if (!collect_positions(table, where, &matched, error, error_size)) {
        return 0;
    }

    if (!check_keys_after_update(table, &matched, &changes,
                                 error, error_size)) {
        free(matched.positions);
        return 0;
    }

    size_t updated = 0;

    for (size_t i = 0; i < matched.count; i++) {
        for (size_t c = 0; c < changes.columns.count; c++) {
            // Only text needs memory. Running out partway leaves the
            // changes made so far in place; there's no undo.
            if (!table_set_value(table, matched.positions[i],
                                 changes.columns.index[c],
                                 &changes.values[c])) {
                free(matched.positions);
                return fail(error, error_size,
                            "out of memory after updating %zu rows", updated);
            }
        }
        updated++;
    }

    printf("(%zu %s updated)\n", updated, updated == 1 ? "row" : "rows");
    free(matched.positions);
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
