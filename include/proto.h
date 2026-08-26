/* The client/server wire, and where sessions live.
 *
 * Frames are `u8 type, u32 length (big endian), payload`. Deliberately dull:
 * the interesting protocol is the JSON control channel (M3) that rides on
 * MSG_CMD, and it speaks the same vocabulary as the headless driver.
 */
#ifndef SLOSH_PROTO_H
#define SLOSH_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MSG_HELLO and MSG_RESIZE carry u16 cols, u16 rows, and since the graphics
 * fix two more: u16 cell_w, u16 cell_h, the client's cell size in pixels.
 * A four-byte payload is still accepted and means "I did not say", which is
 * what an older client sends and what a terminal that reports no pixel size
 * leads to. */
enum {
  MSG_HELLO = 1,  /* client -> server: u16 cols, rows[, cell_w, cell_h] */
  MSG_INPUT = 2,  /* client -> server: raw bytes from the terminal */
  MSG_RESIZE = 3, /* client -> server: u16 cols, rows[, cell_w, cell_h] */
  MSG_DETACH = 4, /* client -> server: leave, but keep running */
  MSG_OUTPUT = 5, /* server -> client: bytes for the terminal */
  MSG_EXIT = 6,   /* server -> client: u8 reason */
  MSG_CMD = 7,    /* client -> server: one JSON line */
  MSG_REPLY = 8,  /* server -> client: one JSON line */
};

enum {
  EXIT_SESSION_ENDED = 0, /* the last pane closed */
  EXIT_DETACHED = 1,      /* the user detached */
  EXIT_REPLACED = 2,      /* another client attached */
};

typedef struct {
  uint8_t type;
  uint8_t *data;
  uint32_t len;
} msg_t;

/* Incremental framing: feed bytes, take whole messages out. */
typedef struct {
  uint8_t *buf;
  size_t len, cap;
  uint8_t *msg; /* the payload handed to the caller, lifted out of buf */
  size_t msg_cap;
} msg_reader_t;

void msg_reader_init(msg_reader_t *r);
void msg_reader_free(msg_reader_t *r);
void msg_reader_feed(msg_reader_t *r, const uint8_t *data, size_t len);
/* Returns true and fills `out` (data points into the reader's buffer, valid
 * until the next call) when a whole message is available. */
bool msg_reader_next(msg_reader_t *r, msg_t *out);

/* One frame is a type byte and a 32-bit big-endian length, then the payload.
 * The server frames into its own queue (it cannot afford to block on a client
 * that has stopped reading), so the size is not private to proto.c. */
#define MSG_HDR 5

/* Blocking send of one frame. Returns 0 on success, -1 if the peer is gone.
 * For the client, which has nothing else to be doing while it writes. */
int msg_send(int fd, uint8_t type, const void *data, size_t len);

/* $XDG_RUNTIME_DIR/slosh/<name>.sock, or /tmp/slosh-<uid>/<name>.sock.
 * Creates the directory (0700). Returns 0 on success. */
int session_socket_path(const char *name, char *out, size_t cap);
int session_log_path(const char *name, char *out, size_t cap);
/* Names of live sessions (sockets that answer). Caller frees each and the
 * array. Returns the count. */
size_t session_list(char ***out_names);

#endif /* SLOSH_PROTO_H */
