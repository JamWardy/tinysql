#include "protocol.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The type byte for a value or column type.
enum {
    WIRE_NULL = 0,
    WIRE_INT = 1,
    WIRE_TEXT = 2,
    WIRE_BOOL = 3,
};

// ---- Buffers ----

void buffer_init(Buffer *buffer) {
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

void buffer_free(Buffer *buffer) {
    free(buffer->data);
    buffer_init(buffer);
}

int buffer_append(Buffer *buffer, const void *bytes, size_t count) {
    if (count > SIZE_MAX - buffer->length) {
        return 0;
    }

    size_t needed = buffer->length + count;

    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;

        while (capacity < needed) {
            capacity = capacity > SIZE_MAX / 2 ? needed : capacity * 2;
        }

        unsigned char *data = realloc(buffer->data, capacity);
        if (!data) {
            return 0;
        }
        buffer->data = data;
        buffer->capacity = capacity;
    }

    if (count > 0) {
        memcpy(buffer->data + buffer->length, bytes, count);
    }
    buffer->length = needed;
    return 1;
}

void buffer_consume(Buffer *buffer, size_t count) {
    memmove(buffer->data, buffer->data + count, buffer->length - count);
    buffer->length -= count;
}

// ---- Reading ----

static uint64_t decode_uint(const unsigned char *bytes, size_t size) {
    uint64_t n = 0;

    for (size_t i = 0; i < size; i++) {
        n = (n << 8) | bytes[i];
    }
    return n;
}

FrameStatus protocol_next_frame(const Buffer *in, unsigned char *type,
                                size_t *body_length) {
    if (in->length < PROTOCOL_HEADER_SIZE) {
        return FRAME_INCOMPLETE;
    }

    uint64_t length = decode_uint(in->data + 1, 4);

    if (length > PROTOCOL_MAX_BODY) {
        return FRAME_TOO_BIG;
    }
    if (in->length - PROTOCOL_HEADER_SIZE < length) {
        return FRAME_INCOMPLETE;
    }

    *type = in->data[0];
    *body_length = (size_t)length;
    return FRAME_READY;
}

// A message body, read from the front. Every read checks there's enough left,
// as the client may send anything.
typedef struct {
    const unsigned char *next;
    size_t remaining;
} BodyReader;

static int fail_short(Error *error) {
    return fail(error, ERROR_PROTOCOL_VIOLATION, "message is cut short");
}

static int read_uint(BodyReader *r, size_t size, uint64_t *out,
                     Error *error) {
    if (r->remaining < size) {
        return fail_short(error);
    }
    *out = decode_uint(r->next, size);
    r->next += size;
    r->remaining -= size;
    return 1;
}

// The two's complement bits as a signed number, without converting an
// out-of-range unsigned value, which C leaves to the compiler.
static long long int_from_bits(uint64_t bits) {
    if (bits <= (uint64_t)LLONG_MAX) {
        return (long long)bits;
    }
    return -(long long)(~bits) - 1;
}

// u32 length + bytes, as a '\0'-terminated copy. TinySQL's text ends at a
// '\0', so one inside would cut it short.
static int read_text(BodyReader *r, char **out, Error *error) {
    uint64_t length;

    if (!read_uint(r, 4, &length, error)) {
        return 0;
    }
    if (r->remaining < length) {
        return fail_short(error);
    }
    if (memchr(r->next, '\0', (size_t)length)) {
        return fail(error, ERROR_PROTOCOL_VIOLATION,
                    "text can't contain a NUL byte");
    }

    char *text = malloc((size_t)length + 1);
    if (!text) {
        return fail(error, ERROR_OUT_OF_MEMORY, "out of memory");
    }
    memcpy(text, r->next, (size_t)length);
    text[length] = '\0';

    r->next += length;
    r->remaining -= (size_t)length;
    *out = text;
    return 1;
}

static int read_value(BodyReader *r, Value *out, Error *error) {
    uint64_t type;
    uint64_t bits;

    if (!read_uint(r, 1, &type, error)) {
        return 0;
    }

    switch (type) {
        case WIRE_NULL:
            out->type = TYPE_NULL;
            return 1;
        case WIRE_INT:
            if (!read_uint(r, 8, &bits, error)) {
                return 0;
            }
            out->type = TYPE_INT;
            out->as.integer = int_from_bits(bits);
            return 1;
        case WIRE_TEXT:
            if (!read_text(r, &out->as.text, error)) {
                return 0;
            }
            out->type = TYPE_TEXT;
            return 1;
        case WIRE_BOOL:
            if (!read_uint(r, 1, &bits, error)) {
                return 0;
            }
            out->type = TYPE_BOOL;
            out->as.boolean = bits != 0;
            return 1;
    }
    return fail(error, ERROR_PROTOCOL_VIOLATION, "unknown value type %llu",
                (unsigned long long)type);
}

int protocol_read_query(const unsigned char *body, size_t length, Query *out,
                        Error *error) {
    BodyReader r = { body, length };
    uint64_t count;

    out->sql = NULL;
    out->params = NULL;
    out->param_count = 0;

    if (!read_text(&r, &out->sql, error) ||
        !read_uint(&r, 2, &count, error)) {
        query_free(out);
        return 0;
    }

    if (count > 0) {
        out->params = calloc((size_t)count, sizeof(Value));
        if (!out->params) {
            query_free(out);
            return fail(error, ERROR_OUT_OF_MEMORY, "out of memory");
        }
    }

    // param_count counts only values read, so query_free frees just those.
    while (out->param_count < count) {
        if (!read_value(&r, &out->params[out->param_count], error)) {
            query_free(out);
            return 0;
        }
        out->param_count++;
    }

    if (r.remaining != 0) {
        query_free(out);
        return fail(error, ERROR_PROTOCOL_VIOLATION,
                    "query message has %zu bytes left over", r.remaining);
    }
    return 1;
}

void query_free(Query *query) {
    free(query->sql);
    values_free(query->params, query->param_count);
    free(query->params);
    query->sql = NULL;
    query->params = NULL;
    query->param_count = 0;
}

// ---- Writing ----

static int append_uint(Buffer *out, uint64_t n, size_t size) {
    unsigned char bytes[8];

    for (size_t i = 0; i < size; i++) {
        bytes[i] = (unsigned char)(n >> (8 * (size - 1 - i)));
    }
    return buffer_append(out, bytes, size);
}

// Writes the type and a length to fill in once the body is written. Returns
// where the message starts, for end_message.
static int begin_message(Buffer *out, MessageType type, size_t *start) {
    *start = out->length;
    return append_uint(out, (uint64_t)type, 1) && append_uint(out, 0, 4);
}

static void end_message(Buffer *out, size_t start) {
    uint64_t length = out->length - start - PROTOCOL_HEADER_SIZE;

    for (size_t i = 0; i < 4; i++) {
        out->data[start + 1 + i] = (unsigned char)(length >> (8 * (3 - i)));
    }
}

static int append_text(Buffer *out, const char *text, size_t length_size) {
    size_t length = strlen(text);

    return append_uint(out, length, length_size) &&
           buffer_append(out, text, length);
}

static uint64_t wire_type(DataType type) {
    switch (type) {
        case TYPE_NULL:
            return WIRE_NULL;
        case TYPE_INT:
            return WIRE_INT;
        case TYPE_TEXT:
            return WIRE_TEXT;
        case TYPE_BOOL:
            return WIRE_BOOL;
    }
    return WIRE_NULL;
}

static int append_value(Buffer *out, const Value *value) {
    if (!append_uint(out, wire_type(value->type), 1)) {
        return 0;
    }

    switch (value->type) {
        case TYPE_NULL:
            return 1;
        case TYPE_INT:
            return append_uint(out, (uint64_t)value->as.integer, 8);
        case TYPE_TEXT:
            return append_text(out, value->as.text, 4);
        case TYPE_BOOL:
            return append_uint(out, value->as.boolean ? 1 : 0, 1);
    }
    return 0;
}

static int write_columns(Buffer *out, const Result *result) {
    size_t start;

    if (!begin_message(out, MESSAGE_COLUMNS, &start) ||
        !append_uint(out, result->column_count, 2)) {
        return 0;
    }
    for (size_t i = 0; i < result->column_count; i++) {
        if (!append_text(out, result->names[i], 2) ||
            !append_uint(out, wire_type(result->types[i]), 1)) {
            return 0;
        }
    }
    end_message(out, start);
    return 1;
}

static int write_row(Buffer *out, const Result *result, size_t row) {
    size_t start;

    if (!begin_message(out, MESSAGE_ROW, &start)) {
        return 0;
    }
    for (size_t i = 0; i < result->column_count; i++) {
        if (!append_value(out, result_value(result, row, i))) {
            return 0;
        }
    }
    end_message(out, start);
    return 1;
}

int protocol_write_result(Buffer *out, const Result *result) {
    size_t start;

    if (result->column_count > 0) {
        if (!write_columns(out, result)) {
            return 0;
        }
        for (size_t row = 0; row < result->row_count; row++) {
            if (!write_row(out, result, row)) {
                return 0;
            }
        }
    }

    if (!begin_message(out, MESSAGE_COMPLETE, &start) ||
        !append_uint(out, result->changed, 8)) {
        return 0;
    }
    end_message(out, start);
    return 1;
}

int protocol_write_error(Buffer *out, const Error *error) {
    size_t start;

    if (!begin_message(out, MESSAGE_ERROR, &start) ||
        !buffer_append(out, error_sqlstate(error->code), 5) ||
        !append_text(out, error->message, 4)) {
        return 0;
    }
    end_message(out, start);
    return 1;
}

int protocol_write_ready(Buffer *out, int in_transaction) {
    size_t start;
    unsigned char status = in_transaction ? 'T' : 'I';

    if (!begin_message(out, MESSAGE_READY, &start) ||
        !buffer_append(out, &status, 1)) {
        return 0;
    }
    end_message(out, start);
    return 1;
}
