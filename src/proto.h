/* The client/server wire, and where sessions live.
 *
 * Frames are `u8 type, u32 length (big endian), payload`. Deliberately dull:
 * the interesting protocol is the JSON control channel (M3) that rides on
 * MSG_CMD, and it speaks the same vocabulary as the headless driver.
 */
#ifndef SL0PPTY_PROTO_H
#define SL0PPTY_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  MSG_HELLO = 1,  /* client -> server: u16 cols, u16 rows */
  MSG_INPUT = 2,  /* client -> server: raw bytes from the terminal */
  MSG_RESIZE = 3, /* client -> server: u16 cols, u16 rows */
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

/* Blocking send of one frame. Returns 0 on success, -1 if the peer is gone. */
int msg_send(int fd, uint8_t type, const void *data, size_t len);

/* $XDG_RUNTIME_DIR/sl0ppty/<name>.sock, or /tmp/sl0ppty-<uid>/<name>.sock.
 * Creates the directory (0700). Returns 0 on success. */
int session_socket_path(const char *name, char *out, size_t cap);
int session_log_path(const char *name, char *out, size_t cap);
/* Names of live sessions (sockets that answer). Caller frees each and the
 * array. Returns the count. */
size_t session_list(char ***out_names);

#endif /* SL0PPTY_PROTO_H */
