#include "persist.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"

// File layout. Integers are unsigned and little-endian, written a byte at
// a time so the file is the same on every machine.
//
//   "TSQL"  u32 version  u32 table_count
//   per table:   string name  u32 column_count
//                per column: u8 type  string name
//                u64 row_count
//                per row, per column: INT as u64, TEXT as string
//
// A string is a u32 byte count followed by the bytes, with no '\0'.
// Negative INTs are stored as their two's complement.

#define MAGIC "TSQL"
#define MAGIC_LENGTH 4
#define FORMAT_VERSION 1

// Type codes in the file. Fixed here rather than taken from DataType,
// so reordering that enum can't change the meaning of existing files.
#define FILE_TYPE_INT  1
#define FILE_TYPE_TEXT 2

// ---- Saving ----

// Write errors set the stream's error flag, which save checks once at
// the end, so these don't report failures themselves.
static void write_uint(FILE *file, uint64_t n, int bytes) {
    for (int i = 0; i < bytes; i++) {
        fputc((int)((n >> (8 * i)) & 0xFF), file);
    }
}

static void write_string(FILE *file, const char *s) {
    size_t length = strlen(s);

    assert(length <= UINT32_MAX);
    write_uint(file, length, 4);
    fwrite(s, 1, length, file);
}

static void write_table(FILE *file, const Table *table) {
    write_string(file, table->name);
    write_uint(file, table->column_count, 4);

    for (size_t i = 0; i < table->column_count; i++) {
        const Column *column = &table->columns[i];

        write_uint(file, column->type == TYPE_INT
                             ? FILE_TYPE_INT : FILE_TYPE_TEXT, 1);
        write_string(file, column->name);
    }

    write_uint(file, table->row_count, 8);

    for (size_t r = 0; r < table->row_count; r++) {
        const Row *row = &table->rows[r];

        for (size_t i = 0; i < row->count; i++) {
            if (row->values[i].type == TYPE_INT) {
                write_uint(file, (uint64_t)row->values[i].as.integer, 8);
            } else {
                write_string(file, row->values[i].as.text);
            }
        }
    }
}

int database_save(const Database *db, const char *path,
                  char *error, size_t error_size) {
    // Write a complete copy beside the real file, then rename it over
    // the real one, which swaps the file in a single step.
    size_t length = strlen(path);
    char *temp_path = malloc(length + sizeof(".tmp"));

    if (!temp_path) {
        return fail(error, error_size, "out of memory");
    }
    memcpy(temp_path, path, length);
    memcpy(temp_path + length, ".tmp", sizeof(".tmp"));

    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        fail(error, error_size, "cannot create '%s': %s",
             temp_path, strerror(errno));
        free(temp_path);
        return 0;
    }

    fwrite(MAGIC, 1, MAGIC_LENGTH, file);
    write_uint(file, FORMAT_VERSION, 4);
    write_uint(file, db->count, 4);

    for (size_t i = 0; i < db->count; i++) {
        write_table(file, &db->tables[i]);
    }

    // fclose writes out anything still buffered, so it can fail too.
    int failed = ferror(file);
    failed |= fclose(file) != 0;

    if (failed) {
        fail(error, error_size, "error writing '%s'", temp_path);
        remove(temp_path);
        free(temp_path);
        return 0;
    }

    if (rename(temp_path, path) != 0) {
        fail(error, error_size, "cannot replace '%s': %s",
             path, strerror(errno));
        remove(temp_path);
        free(temp_path);
        return 0;
    }

    free(temp_path);
    return 1;
}

// ---- Loading ----

// The file is untrusted: anything in it may be wrong, so every count,
// length and type is checked before it's used.
typedef struct {
    FILE *file;
    uint64_t remaining;  // Bytes left in the file; no length can exceed it.
    char *error;
    size_t error_size;
} Reader;

// Records an error message and returns 0, so callers can
// `return read_fail(...)`.
static int read_fail(Reader *r, const char *format, ...) {
    va_list args;

    va_start(args, format);
    vsnprintf(r->error, r->error_size, format, args);
    va_end(args);
    return 0;
}

static int read_bytes(Reader *r, void *out, size_t count) {
    if (count > r->remaining ||
        fread(out, 1, count, r->file) != count) {
        return read_fail(r, "file is truncated or unreadable");
    }

    r->remaining -= count;
    return 1;
}

static int read_uint(Reader *r, int bytes, uint64_t *out) {
    unsigned char buffer[8];

    if (!read_bytes(r, buffer, (size_t)bytes)) {
        return 0;
    }

    uint64_t n = 0;
    for (int i = bytes - 1; i >= 0; i--) {
        n = (n << 8) | buffer[i];
    }

    *out = n;
    return 1;
}

// Converts back from two's complement without the implementation-defined
// cast of a too-large unsigned value to a signed type.
static long long to_signed(uint64_t n) {
    if (n <= (uint64_t)LLONG_MAX) {
        return (long long)n;
    }
    return -(long long)(UINT64_MAX - n) - 1;
}

// Reads a string's byte count, which must be at most max.
static int read_length(Reader *r, uint64_t max, size_t *out) {
    uint64_t length;

    if (!read_uint(r, 4, &length)) {
        return 0;
    }
    if (length > r->remaining) {
        return read_fail(r, "file is truncated or unreadable");
    }
    if (length > max) {
        return read_fail(r, "name too long (max %llu characters)",
                         (unsigned long long)max);
    }

    *out = (size_t)length;
    return 1;
}

// Reads length bytes into out and adds the '\0'. A '\0' inside the data
// would silently cut the string short, so it's rejected.
static int read_chars(Reader *r, char *out, size_t length) {
    if (!read_bytes(r, out, length)) {
        return 0;
    }
    if (memchr(out, '\0', length)) {
        return read_fail(r, "string contains a NUL byte");
    }

    out[length] = '\0';
    return 1;
}

// Reads a table or column name into a buffer of MAX_TOKEN_LENGTH, the
// same limit the tokeniser puts on names typed in SQL.
static int read_name(Reader *r, char *out) {
    size_t length;

    if (!read_length(r, MAX_TOKEN_LENGTH - 1, &length)) {
        return 0;
    }
    if (length == 0) {
        return read_fail(r, "empty name");
    }

    return read_chars(r, out, length);
}

// Reads a TEXT value into a new heap string, which the caller frees.
static int read_text(Reader *r, char **out) {
    size_t length;

    if (!read_length(r, r->remaining, &length)) {
        return 0;
    }

    char *text = malloc(length + 1);
    if (!text) {
        return read_fail(r, "out of memory");
    }

    if (!read_chars(r, text, length)) {
        free(text);
        return 0;
    }

    *out = text;
    return 1;
}

// Frees the TEXT values among the first count values.
static void free_texts(Value *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (values[i].type == TYPE_TEXT) {
            free(values[i].as.text);
        }
    }
}

// Reads one row's values and appends it to table.
static int read_row(Reader *r, Table *table) {
    Value values[MAX_COLUMNS];

    for (size_t i = 0; i < table->column_count; i++) {
        values[i].type = table->columns[i].type;

        int ok;
        if (values[i].type == TYPE_INT) {
            uint64_t n;
            ok = read_uint(r, 8, &n);
            values[i].as.integer = to_signed(n);
        } else {
            ok = read_text(r, &values[i].as.text);
        }

        if (!ok) {
            free_texts(values, i);
            return 0;
        }
    }

    // The table keeps its own copies, so ours are freed either way.
    int ok = table_append_row(table, values);
    free_texts(values, table->column_count);

    return ok ? 1 : read_fail(r, "out of memory");
}

static int read_table(Reader *r, Database *db) {
    char name[MAX_TOKEN_LENGTH];
    ColumnDef columns[MAX_COLUMNS];
    uint64_t column_count;

    if (!read_name(r, name) || !read_uint(r, 4, &column_count)) {
        return 0;
    }

    if (database_find_table(db, name)) {
        return read_fail(r, "table '%s' appears twice", name);
    }

    // The executor sizes its arrays by MAX_COLUMNS, so more would overflow.
    if (column_count == 0 || column_count > MAX_COLUMNS) {
        return read_fail(r,
                         "table '%s' has %llu columns (must be 1 to %d)",
                         name, (unsigned long long)column_count, MAX_COLUMNS);
    }

    for (size_t i = 0; i < column_count; i++) {
        uint64_t type;

        if (!read_uint(r, 1, &type)) {
            return 0;
        }

        if (type == FILE_TYPE_INT) {
            columns[i].type = TYPE_INT;
        } else if (type == FILE_TYPE_TEXT) {
            columns[i].type = TYPE_TEXT;
        } else {
            return read_fail(r,
                             "table '%s' has an unknown column type %llu",
                             name, (unsigned long long)type);
        }

        if (!read_name(r, columns[i].name)) {
            return 0;
        }

        for (size_t j = 0; j < i; j++) {
            if (strcmp(columns[i].name, columns[j].name) == 0) {
                return read_fail(r, "table '%s' has duplicate column '%s'",
                                 name, columns[i].name);
            }
        }
    }

    Table *table = database_create_table(db, name, columns,
                                         (size_t)column_count);
    if (!table) {
        return read_fail(r, "out of memory");
    }

    uint64_t row_count;
    if (!read_uint(r, 8, &row_count)) {
        return 0;
    }

    // No other table is created while reading rows, so table stays valid.
    for (uint64_t i = 0; i < row_count; i++) {
        if (!read_row(r, table)) {
            return 0;
        }
    }

    return 1;
}

static int read_database(Reader *r, Database *db) {
    char magic[MAGIC_LENGTH];
    uint64_t version;
    uint64_t table_count;

    if (!read_bytes(r, magic, MAGIC_LENGTH) ||
        memcmp(magic, MAGIC, MAGIC_LENGTH) != 0) {
        return read_fail(r, "not a TinySQL database file");
    }

    if (!read_uint(r, 4, &version)) {
        return 0;
    }
    if (version != FORMAT_VERSION) {
        return read_fail(r, "unsupported file version %llu (expected %d)",
                         (unsigned long long)version, FORMAT_VERSION);
    }

    if (!read_uint(r, 4, &table_count)) {
        return 0;
    }

    for (uint64_t i = 0; i < table_count; i++) {
        if (!read_table(r, db)) {
            return 0;
        }
    }

    if (r->remaining != 0) {
        return read_fail(r, "unexpected data at end of file");
    }

    return 1;
}

int database_load(Database *db, const char *path,
                  char *error, size_t error_size) {
    assert(db->count == 0);

    errno = 0;
    FILE *file = fopen(path, "rb");

    if (!file) {
        // No file yet: start empty; the first save creates it.
        if (errno == ENOENT) {
            return 1;
        }
        return fail(error, error_size, "%s", strerror(errno));
    }

    Reader r = { file, 0, error, error_size };
    int ok = 1;

    // The file's size bounds every length in it, so a corrupt length
    // can't make us allocate more than the file could hold.
    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        size = ftell(file);
    }
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        ok = read_fail(&r, "cannot read file size");
    } else {
        r.remaining = (uint64_t)size;
        ok = read_database(&r, db);
    }

    fclose(file);

    if (!ok) {
        database_free(db);
    }
    return ok;
}
