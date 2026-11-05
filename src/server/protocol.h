#ifndef TINYSQL_SERVER_PROTOCOL_H
#define TINYSQL_SERVER_PROTOCOL_H

// The messages between the server and its clients, modelled on Postgres's.
// Every message is a type byte, the body's length in 4 bytes, then the body.
// Integers are big-endian, as network protocols' are.
//
// Client to server:
//   'Q' query       a statement: u32 length + SQL, then u16 count + that
//                   many values, one per `?`
//   'X' terminate   empty
//   'R' replicate   empty: this connection is a replica (see below)
//
// Server to client, answering each 'Q' with T, D..., C, Z (T and D only when
// there are rows to give, as for SELECT), or with E, Z:
//   'T' columns     u16 count, then for each: u16 length + name, u8 type
//   'D' row         one value per column
//   'C' complete    u64 rows inserted, updated or deleted
//   'E' error       5-byte SQLSTATE, then u32 length + message
//   'Z' ready       'I' idle, or 'T' in a transaction
//
// Primary to replica, answering 'R' with S, then a U after each commit:
//   'S' snapshot    the primary's database file, byte for byte
//   'U' unit        what one commit changed: u32 count, then that many
//                   statements, as in 'Q'
//
// A value, or a column's type, starts with a u8 type: 0 NULL, 1 INT, 2 TEXT,
// 3 BOOL. Then a value has nothing for NULL, 8 bytes for INT, u32 length +
// bytes for TEXT, and 1 byte for BOOL.

#include <stddef.h>

#include "error.h"
#include "result.h"
#include "value.h"

#define PROTOCOL_HEADER_SIZE 5

// A bigger message from a client is refused, so a client can't make the
// server allocate whatever it likes.
#define PROTOCOL_MAX_BODY (1024 * 1024)

// The primary is trusted, and its snapshot is the whole database, so a
// replica takes whatever the 4-byte length can say.
#define PROTOCOL_MAX_FROM_PRIMARY 0xFFFFFFFFu

typedef enum {
    MESSAGE_QUERY = 'Q',
    MESSAGE_TERMINATE = 'X',
    MESSAGE_REPLICATE = 'R',
    MESSAGE_COLUMNS = 'T',
    MESSAGE_ROW = 'D',
    MESSAGE_COMPLETE = 'C',
    MESSAGE_ERROR = 'E',
    MESSAGE_READY = 'Z',
    MESSAGE_SNAPSHOT = 'S',
    MESSAGE_UNIT = 'U',
} MessageType;

// Bytes received but not yet handled, or waiting to be sent.
typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} Buffer;

void buffer_init(Buffer *buffer);
void buffer_free(Buffer *buffer);

// Fails only if memory ran out, leaving buffer as it was.
int buffer_append(Buffer *buffer, const void *bytes, size_t count);

// Drops the first count bytes, once they're handled or sent.
void buffer_consume(Buffer *buffer, size_t count);

typedef enum {
    FRAME_INCOMPLETE,  // Wait for more bytes.
    FRAME_READY,       // The first message is all there.
    FRAME_TOO_BIG,     // Over the limit given.
} FrameStatus;

// Whether in starts with a whole message, and if so its type and body length.
// The body starts at PROTOCOL_HEADER_SIZE.
FrameStatus protocol_next_frame(const Buffer *in, size_t max_body,
                                unsigned char *type, size_t *body_length);

// A 'Q' message's contents. It owns sql, params and the params' text.
typedef struct {
    char *sql;
    Value *params;
    size_t param_count;
} Query;

// On failure out is left empty, with nothing to free.
int protocol_read_query(const unsigned char *body, size_t length, Query *out,
                        Error *error);

void query_free(Query *query);

// A 'U' message's statements, in the order they ran. It owns them.
typedef struct {
    Query *statements;
    size_t count;
} Unit;

// On failure out is left empty, with nothing to free.
int protocol_read_unit(const unsigned char *body, size_t length, Unit *out,
                       Error *error);

void unit_free(Unit *unit);

// Each appends whole messages to out, and fails only if memory ran out, in
// which case out may hold part of them.

// T and D only if result has columns, then C.
int protocol_write_result(Buffer *out, const Result *result);
int protocol_write_error(Buffer *out, const Error *error);
int protocol_write_ready(Buffer *out, int in_transaction);

int protocol_write_replicate(Buffer *out);
int protocol_write_snapshot(Buffer *out, const Buffer *file);

// Adds one statement to statements, the body of a unit being gathered.
int protocol_append_statement(Buffer *statements, const char *sql,
                              const Value *params, size_t param_count);

// A U holding count statements, as protocol_append_statement left them.
int protocol_write_unit(Buffer *out, const Buffer *statements, size_t count);

#endif
