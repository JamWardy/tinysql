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

// File layout. Integers are unsigned and little-endian, written a byte at a
// time so the file is the same on every machine.
//
//   "TSQL"  u32 version  u32 table_count
//   per table:   string name  u32 column_count
//                per column: u8 type  u8 flags  string name
//                u8 constraint_count
//                per constraint: u8 kind  u8 column_count
//                                per column: u8 position
//                u8 autoincrement  u64 last_id
//                u8 index_count
//                per index:  string name  u8 column_count
//                            per column: u8 position
//                u64 row_count
//                per row:    null bitmap
//                            per non-null value: INT as u64, TEXT as string
//
// A string is a u32 byte count, then the bytes, with no '\0'. Negative INTs are
// stored as their two's complement.
//
// The null bitmap has a bit per column, rounded up to whole bytes: bit i
// (bitmap[i / 8] & (1 << (i % 8))) set means column i is NULL, and a NULL has
// no bytes of its own. Bits past the last column are 0, as is a NOT NULL
// column's.
//
// A column's flags byte has bit 0 set for NOT NULL; its other bits are 0.
//
// A constraint's kind is 1 for PRIMARY KEY or 2 for UNIQUE. Its columns, and an
// index's, are positions in the table, in key order. A table has at most one
// PRIMARY KEY, and no two rows with the same key, leaving out UNIQUE keys with
// a NULL.
//
// autoincrement is 1 if INSERT chooses IDs for the PRIMARY KEY, which must then
// be one INT column. last_id is at least every ID in the rows, and 0 without
// autoincrement.
//
// Index names are unique, and no table has one. Indexes, hash and B-tree,
// aren't stored: they're rebuilt as the rows are read.
//
// Older versions are still read, and are saved as version 7:
//   version 6 has no indexes.
//   version 5 also has no AUTOINCREMENT, and no last_id.
//   version 4 also has no UNIQUE.
//   version 3 has no constraints.
//   version 2 also has no flags byte: no column is NOT NULL.
//   version 1 also has no bitmap: nothing is NULL.

#define MAGIC "TSQL"
#define MAGIC_LENGTH 4
#define FORMAT_VERSION 7

// Type codes in the file. Fixed here rather than taken from DataType,
// so reordering that enum can't change the meaning of existing files.
#define FILE_TYPE_INT  1
#define FILE_TYPE_TEXT 2

// Bits in a column's flags byte.
#define FILE_FLAG_NOT_NULL 1

// Constraint kinds in the file, fixed like the type codes.
#define FILE_CONSTRAINT_PRIMARY_KEY 1
#define FILE_CONSTRAINT_UNIQUE      2

#define BITMAP_SIZE(column_count) (((column_count) + 7) / 8)

// Room for the most columns a table can have; only the first
// BITMAP_SIZE(column_count) bytes go in the file.
typedef struct {
    unsigned char bytes[BITMAP_SIZE(MAX_COLUMNS)];
} NullBitmap;

static void set_null_bit(NullBitmap *bitmap, size_t i) {
    bitmap->bytes[i / 8] |= (unsigned char)(1 << (i % 8));
}

static int null_bit_is_set(const NullBitmap *bitmap, size_t i) {
    return (bitmap->bytes[i / 8] >> (i % 8)) & 1;
}

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

static void write_row(FILE *file, const Row *row) {
    NullBitmap bitmap = { { 0 } };

    for (size_t i = 0; i < row->count; i++) {
        if (row->values[i].type == TYPE_NULL) {
            set_null_bit(&bitmap, i);
        }
    }
    fwrite(bitmap.bytes, 1, BITMAP_SIZE(row->count), file);

    for (size_t i = 0; i < row->count; i++) {
        const Value *value = &row->values[i];

        if (value->type == TYPE_INT) {
            write_uint(file, (uint64_t)value->as.integer, 8);
        } else if (value->type == TYPE_TEXT) {
            write_string(file, value->as.text);
        }
    }
}

static void write_table(FILE *file, const Table *table) {
    write_string(file, table->name);
    write_uint(file, table->column_count, 4);

    for (size_t i = 0; i < table->column_count; i++) {
        const Column *column = &table->columns[i];

        write_uint(file, column->type == TYPE_INT
                             ? FILE_TYPE_INT : FILE_TYPE_TEXT, 1);
        write_uint(file, column->not_null ? FILE_FLAG_NOT_NULL : 0, 1);
        write_string(file, column->name);
    }

    assert(table->constraint_count <= UINT8_MAX);
    write_uint(file, table->constraint_count, 1);

    for (size_t i = 0; i < table->constraint_count; i++) {
        const Constraint *constraint = &table->constraints[i];

        switch (constraint->kind) {
            case CONSTRAINT_PRIMARY_KEY:
                write_uint(file, FILE_CONSTRAINT_PRIMARY_KEY, 1);
                break;
            case CONSTRAINT_UNIQUE:
                write_uint(file, FILE_CONSTRAINT_UNIQUE, 1);
                break;
        }

        // At most MAX_COLUMNS, so a byte is plenty for each.
        write_uint(file, constraint->columns.count, 1);
        for (size_t c = 0; c < constraint->columns.count; c++) {
            write_uint(file, constraint->columns.positions[c], 1);
        }
    }

    write_uint(file, table->autoincrement ? 1 : 0, 1);
    write_uint(file, (uint64_t)table->last_id, 8);

    write_uint(file, table->index_count, 1);
    for (size_t i = 0; i < table->index_count; i++) {
        const TreeIndex *index = &table->indexes[i];

        write_string(file, index->name);
        write_uint(file, index->columns.count, 1);
        for (size_t c = 0; c < index->columns.count; c++) {
            write_uint(file, index->columns.positions[c], 1);
        }
    }
    write_uint(file, table->row_count, 8);

    for (size_t r = 0; r < table->row_count; r++) {
        write_row(file, &table->rows[r]);
    }
}

// path with ".tmp" on the end, or NULL if memory ran out.
static char *make_temp_path(const char *path) {
    size_t length = strlen(path);
    char *temp_path = malloc(length + sizeof(".tmp"));

    if (temp_path) {
        memcpy(temp_path, path, length);
        memcpy(temp_path + length, ".tmp", sizeof(".tmp"));
    }
    return temp_path;
}

// After a failed save: deletes the half-written temp file and frees its
// path. Call it after writing the error, since the error may use errno
// or temp_path.
static void discard_temp(char *temp_path) {
    remove(temp_path);
    free(temp_path);
}

int database_save(const Database *db, const char *path,
                  char *error, size_t error_size) {
    // Write a complete copy beside the real file, then rename it over
    // the real one, which swaps the file in a single step.
    char *temp_path = make_temp_path(path);

    if (!temp_path) {
        return fail(error, error_size, "out of memory");
    }

    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        fail(error, error_size, "cannot create '%s': %s",
             temp_path, strerror(errno));
        free(temp_path);  // Nothing was created, so nothing to remove.
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
        discard_temp(temp_path);
        return 0;
    }

    if (rename(temp_path, path) != 0) {
        fail(error, error_size, "cannot replace '%s': %s",
             path, strerror(errno));
        discard_temp(temp_path);
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
    uint64_t version;    // The file's FORMAT_VERSION, once it's read.
    char *error;
    size_t error_size;
} Reader;

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

// At most max.
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

// Adds the '\0'. A '\0' inside the data would silently cut the string short, so
// it's rejected.
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

// Callers pass MAX_TOKEN_LENGTH arrays, the tokeniser's limit on names typed in
// SQL.
static int read_name(Reader *r, char *out, size_t size) {
    size_t length;

    if (!read_length(r, size - 1, &length)) {
        return 0;
    }
    if (length == 0) {
        return read_fail(r, "empty name");
    }

    return read_chars(r, out, length);
}

// Into a new heap string, which the caller frees.
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

// Version 1 rows have none: nothing is NULL.
static int read_bitmap(Reader *r, const Table *table, NullBitmap *out) {
    size_t size = BITMAP_SIZE(table->column_count);

    memset(out, 0, sizeof(*out));
    if (r->version == 1) {
        return 1;
    }

    if (!read_bytes(r, out->bytes, size)) {
        return 0;
    }

    for (size_t i = table->column_count; i < size * 8; i++) {
        if (null_bit_is_set(out, i)) {
            return read_fail(r, "table '%s' has a row with a bad null bitmap",
                             table->name);
        }
    }

    for (size_t i = 0; i < table->column_count; i++) {
        if (null_bit_is_set(out, i) && table->columns[i].not_null) {
            return read_fail(r, "table '%s' has a NULL in NOT NULL column '%s'",
                             table->name, table->columns[i].name);
        }
    }
    return 1;
}

static int read_row(Reader *r, Table *table) {
    NullBitmap bitmap;
    Value values[MAX_COLUMNS];

    if (!read_bitmap(r, table, &bitmap)) {
        return 0;
    }

    for (size_t i = 0; i < table->column_count; i++) {
        if (null_bit_is_set(&bitmap, i)) {
            values[i].type = TYPE_NULL;
            continue;
        }

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
            values_free(values, i);
            return 0;
        }
    }

    size_t id_column;

    if (table_autoincrement_column(table, &id_column) &&
        values[id_column].as.integer > table->last_id) {
        values_free(values, table->column_count);
        return read_fail(r, "table '%s' has an ID above its last ID",
                         table->name);
    }

    for (size_t i = 0; i < table->constraint_count; i++) {
        size_t other;

        if (table_find_key(table, &table->constraints[i], values, &other)) {
            values_free(values, table->column_count);
            return read_fail(r, "table '%s' has two rows with the same key",
                             table->name);
        }
    }

    // The table keeps its own copies, so ours are freed either way.
    int ok = table_append_row(table, values);
    values_free(values, table->column_count);

    return ok ? 1 : read_fail(r, "out of memory");
}

// Files before version 3 have no flags byte: no column is NOT NULL.
static int read_column(Reader *r, const char *table_name, ColumnDef *out) {
    uint64_t type;
    uint64_t flags = 0;

    if (!read_uint(r, 1, &type)) {
        return 0;
    }

    if (type == FILE_TYPE_INT) {
        out->type = TYPE_INT;
    } else if (type == FILE_TYPE_TEXT) {
        out->type = TYPE_TEXT;
    } else {
        return read_fail(r, "table '%s' has an unknown column type %llu",
                         table_name, (unsigned long long)type);
    }

    if (r->version >= 3 && !read_uint(r, 1, &flags)) {
        return 0;
    }
    if (flags & ~(uint64_t)FILE_FLAG_NOT_NULL) {
        return read_fail(r, "table '%s' has unknown column flags %llu",
                         table_name, (unsigned long long)flags);
    }
    out->not_null = (flags & FILE_FLAG_NOT_NULL) != 0;

    return read_name(r, out->name, sizeof(out->name));
}

// Checks each is one of table's columns, named once. what is "key" or "index",
// for messages.
static int read_key_columns(Reader *r, const Table *table, const char *what,
                            ColumnList *out) {
    uint64_t count;

    if (!read_uint(r, 1, &count)) {
        return 0;
    }
    if (count == 0 || count > table->column_count) {
        return read_fail(r, "table '%s' has a %s with %llu columns",
                         table->name, what, (unsigned long long)count);
    }

    out->count = (size_t)count;
    for (size_t i = 0; i < out->count; i++) {
        uint64_t position;

        if (!read_uint(r, 1, &position)) {
            return 0;
        }
        if (position >= table->column_count) {
            return read_fail(r, "table '%s' has a %s with no column %llu",
                             table->name, what,
                             (unsigned long long)position);
        }
        out->positions[i] = (size_t)position;

        for (size_t j = 0; j < i; j++) {
            if (out->positions[j] == out->positions[i]) {
                return read_fail(r, "table '%s' has a %s with column '%s' "
                                 "twice", table->name, what,
                                 table->columns[position].name);
            }
        }
    }

    return 1;
}

// They come before the rows, so each row is checked against them as it's read.
// Files before version 4 have none.
static int read_constraints(Reader *r, Table *table) {
    uint64_t count;

    if (r->version < 4) {
        return 1;
    }
    if (!read_uint(r, 1, &count)) {
        return 0;
    }

    for (uint64_t i = 0; i < count; i++) {
        uint64_t code;
        ConstraintKind kind;
        ColumnList columns;

        if (!read_uint(r, 1, &code)) {
            return 0;
        }
        if (code == FILE_CONSTRAINT_PRIMARY_KEY) {
            kind = CONSTRAINT_PRIMARY_KEY;
        } else if (code == FILE_CONSTRAINT_UNIQUE && r->version >= 5) {
            kind = CONSTRAINT_UNIQUE;
        } else {
            return read_fail(r, "table '%s' has an unknown constraint kind "
                             "%llu", table->name, (unsigned long long)code);
        }

        if (kind == CONSTRAINT_PRIMARY_KEY) {
            for (size_t c = 0; c < table->constraint_count; c++) {
                if (table->constraints[c].kind == CONSTRAINT_PRIMARY_KEY) {
                    return read_fail(r, "table '%s' has more than one "
                                     "primary key", table->name);
                }
            }
        }

        if (!read_key_columns(r, table, "key", &columns)) {
            return 0;
        }
        if (!table_add_constraint(table, kind, &columns)) {
            return read_fail(r, "out of memory");
        }
    }

    return 1;
}

// Files before version 6 have neither. A table that chooses IDs must have a
// PRIMARY KEY of one INT column.
static int read_autoincrement(Reader *r, Table *table) {
    uint64_t autoincrement = 0;
    uint64_t last_id = 0;

    if (r->version >= 6 &&
        (!read_uint(r, 1, &autoincrement) || !read_uint(r, 8, &last_id))) {
        return 0;
    }
    if (autoincrement > 1 || last_id > LLONG_MAX ||
        (!autoincrement && last_id != 0)) {
        return read_fail(r, "table '%s' has a bad AUTOINCREMENT or last ID",
                         table->name);
    }

    table->autoincrement = (int)autoincrement;
    table->last_id = (long long)last_id;

    if (!table->autoincrement) {
        return 1;
    }

    const Constraint *key = NULL;

    for (size_t k = 0; k < table->constraint_count; k++) {
        if (table->constraints[k].kind == CONSTRAINT_PRIMARY_KEY) {
            key = &table->constraints[k];
        }
    }
    if (!key || key->columns.count != 1 ||
        table->columns[key->columns.positions[0]].type != TYPE_INT) {
        return read_fail(r, "table '%s' has AUTOINCREMENT, but its "
                         "PRIMARY KEY isn't one INT column", table->name);
    }
    return 1;
}

// Files before version 7 have none. Each is added while the table is empty, so
// it fills as the rows are read.
static int read_indexes(Reader *r, Table *table) {
    uint64_t count;

    if (r->version < 7) {
        return 1;
    }
    if (!read_uint(r, 1, &count)) {
        return 0;
    }
    if (count > MAX_INDEXES) {
        return read_fail(r, "table '%s' has %llu indexes (max %d)",
                         table->name, (unsigned long long)count, MAX_INDEXES);
    }

    for (uint64_t i = 0; i < count; i++) {
        char name[MAX_TOKEN_LENGTH];
        ColumnList columns;

        if (!read_name(r, name, sizeof(name)) ||
            !read_key_columns(r, table, "index", &columns)) {
            return 0;
        }
        if (!table_add_index(table, name, &columns)) {
            return read_fail(r, "out of memory");
        }
    }
    return 1;
}

// Tables and indexes share names. Run once every table is read, as a later
// table may have the name.
static int check_index_names(Reader *r, const Database *db) {
    for (size_t t = 0; t < db->count; t++) {
        const Table *table = &db->tables[t];

        for (size_t i = 0; i < table->index_count; i++) {
            const char *name = table->indexes[i].name;
            size_t other_table;
            size_t other_index;

            database_find_index(db, name, &other_table, &other_index);
            if (database_find_table(db, name) ||
                other_table != t || other_index != i) {
                return read_fail(r, "the name '%s' is used twice", name);
            }
        }
    }
    return 1;
}

static int read_table(Reader *r, Database *db) {
    char name[MAX_TOKEN_LENGTH];
    ColumnDef columns[MAX_COLUMNS];
    uint64_t column_count;

    if (!read_name(r, name, sizeof(name)) ||
        !read_uint(r, 4, &column_count)) {
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
        if (!read_column(r, name, &columns[i])) {
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

    // No other table is created while reading constraints and rows, so
    // table stays valid.
    if (!read_constraints(r, table) || !read_autoincrement(r, table) ||
        !read_indexes(r, table)) {
        return 0;
    }

    uint64_t row_count;
    if (!read_uint(r, 8, &row_count)) {
        return 0;
    }

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
    if (version < 1 || version > FORMAT_VERSION) {
        return read_fail(r,
                         "unsupported file version %llu (expected 1 to %d)",
                         (unsigned long long)version, FORMAT_VERSION);
    }
    r->version = version;

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

    return check_index_names(r, db);
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

    Reader r = { file, 0, 0, error, error_size };
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
