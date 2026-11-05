#include "internal.h"

#include <stdlib.h>

#include "error.h"

// A run of equal values in a sorted column.
typedef struct {
    size_t start;
    size_t length;
    int common;
} Run;

static int compare_values(const void *a, const void *b) {
    return value_compare(*(const Value *const *)a, *(const Value *const *)b);
}

// Picks the common values: the most frequent runs, as Postgres picks them,
// that occur more than once and more often than average. With few enough
// distinct values, every one is common and nothing is left for a histogram.
static void pick_common(Run *runs, size_t run_count, size_t values) {
    double average = run_count > 0 ? (double)values / (double)run_count : 0;
    int every = run_count <= MAX_COMMON_VALUES;

    for (size_t picked = 0; picked < MAX_COMMON_VALUES; picked++) {
        Run *best = NULL;

        for (size_t i = 0; i < run_count; i++) {
            if (!runs[i].common && (!best || runs[i].length > best->length)) {
                best = &runs[i];
            }
        }
        if (!best || (!every && (best->length < 2 ||
                                 (double)best->length <= average))) {
            return;
        }
        best->common = 1;
    }
}

// Copies the common values and spaces the histogram's bounds evenly through
// the rest. sorted holds the column's non-NULL values in order.
static int fill_stats(ColumnStats *stats, const Value **sorted,
                      const Run *runs, size_t run_count, size_t rows) {
    size_t rest = 0;

    for (size_t i = 0; i < run_count; i++) {
        const Run *run = &runs[i];

        if (!run->common) {
            rest += run->length;
            continue;
        }
        if (!value_copy(&stats->common[stats->common_count],
                        sorted[run->start])) {
            return 0;
        }
        stats->common_fraction[stats->common_count++] =
            (double)run->length / (double)rows;
    }

    // Common values come out in sorted order; most common first is easier
    // to read. An insertion sort, as there are at most ten.
    for (size_t i = 1; i < stats->common_count; i++) {
        for (size_t j = i; j > 0 && stats->common_fraction[j] >
                                    stats->common_fraction[j - 1]; j--) {
            Value value = stats->common[j];
            double fraction = stats->common_fraction[j];

            stats->common[j] = stats->common[j - 1];
            stats->common_fraction[j] = stats->common_fraction[j - 1];
            stats->common[j - 1] = value;
            stats->common_fraction[j - 1] = fraction;
        }
    }

    if (rest < 2) {
        return 1;
    }

    const Value **others = malloc(rest * sizeof(*others));
    size_t count = 0;

    if (!others) {
        return 0;
    }
    for (size_t i = 0; i < run_count; i++) {
        for (size_t j = 0; j < runs[i].length && !runs[i].common; j++) {
            others[count++] = sorted[runs[i].start + j];
        }
    }

    size_t bounds = rest < HISTOGRAM_BUCKETS + 1 ? rest : HISTOGRAM_BUCKETS + 1;
    int ok = 1;

    for (size_t b = 0; b < bounds && ok; b++) {
        size_t at = b * (rest - 1) / (bounds - 1);

        ok = value_copy(&stats->histogram[b], others[at]);
        stats->histogram_count += ok;
    }

    free(others);
    return ok;
}

// Fills stats for column of table from every row. On failure, stats holds
// only what it can free.
static int analyze_column(const Table *table, size_t column,
                          ColumnStats *stats) {
    size_t rows = table->row_count;
    const Value **sorted = malloc((rows > 0 ? rows : 1) * sizeof(*sorted));
    Run *runs = malloc((rows > 0 ? rows : 1) * sizeof(*runs));
    size_t count = 0;
    size_t run_count = 0;
    int ok = sorted && runs;

    for (size_t r = 0; r < rows && ok; r++) {
        const Value *value = &table->rows[r].values[column];

        if (value->type != TYPE_NULL) {
            sorted[count++] = value;
        }
    }

    if (ok) {
        if (count > 1) {
            qsort(sorted, count, sizeof(*sorted), compare_values);
        }
        for (size_t i = 0; i < count; i++) {
            if (i == 0 || value_compare(sorted[i], sorted[i - 1]) != 0) {
                runs[run_count++] = (Run){ i, 0, 0 };
            }
            runs[run_count - 1].length++;
        }

        stats->null_fraction = rows > 0 ? (double)(rows - count) / rows : 0;
        stats->distinct = (double)run_count;
        pick_common(runs, run_count, count);
        ok = fill_stats(stats, sorted, runs, run_count, rows);
    }

    free(sorted);
    free(runs);
    return ok;
}

// Statistics for every column of table, in a new array, or NULL if memory ran
// out.
static ColumnStats *analyze_table(const Table *table) {
    ColumnStats *stats = calloc(table->column_count, sizeof(*stats));

    for (size_t c = 0; stats && c < table->column_count; c++) {
        if (!analyze_column(table, c, &stats[c])) {
            stats_free(stats, table->column_count);
            return NULL;
        }
    }
    return stats;
}

int execute_analyze(Database *db, const Statement *stmt, Error *error) {
    size_t first = 0;
    size_t last = db->count;

    if (stmt->table_name[0] != '\0') {
        Table *table = resolve_table(db, stmt->table_name, error);

        if (!table) {
            return 0;
        }
        first = (size_t)(table - db->tables);
        last = first + 1;
    }

    // Every table's statistics are built before any replace the old, so
    // running out of memory changes nothing.
    ColumnStats **built = malloc((last - first + 1) * sizeof(*built));

    if (!built) {
        return fail(error, ERROR_OUT_OF_MEMORY, "out of memory");
    }
    for (size_t t = first; t < last; t++) {
        built[t - first] = analyze_table(&db->tables[t]);
        if (!built[t - first]) {
            while (t-- > first) {
                stats_free(built[t - first], db->tables[t].column_count);
            }
            free(built);
            return fail(error, ERROR_OUT_OF_MEMORY, "out of memory");
        }
    }
    for (size_t t = first; t < last; t++) {
        table_set_stats(&db->tables[t], built[t - first]);
    }
    free(built);
    return 1;
}
