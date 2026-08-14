/* Input decoding: the bytes the *outer* terminal sends us -> semantic events.
 *
 * libghostty-vt encodes events outward to a pane, but nothing decodes inward,
 * so this half is ours (DESIGN.md). Decoding to a semantic event and
 * re-encoding per pane against that pane's own modes is what makes kitty
 * keyboard passthrough correct instead of approximate.
 *
 * The event carries GhosttyKey / GHOSTTY_MODS_* values as plain integers so
 * this header stays free of libghostty-vt.
 */
#ifndef SL0PPTY_INPUT_H
#define SL0PPTY_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  EV_NONE = 0,
  EV_KEY,
  EV_MOUSE,
  EV_PASTE,
  EV_FOCUS,
} ev_kind_t;

/* mirrors GhosttyKeyAction */
enum { KEY_RELEASE = 0, KEY_PRESS = 1, KEY_REPEAT = 2 };
/* mirrors GhosttyMouseAction */
enum { MOUSE_PRESS = 0, MOUSE_RELEASE = 1, MOUSE_MOTION = 2 };
/* mirrors GhosttyMouseButton */
enum {
  MBTN_UNKNOWN = 0, MBTN_LEFT = 1, MBTN_RIGHT = 2, MBTN_MIDDLE = 3,
  MBTN_FOUR = 4, MBTN_FIVE = 5, MBTN_SIX = 6, MBTN_SEVEN = 7,
};
/* mirrors GHOSTTY_MODS_* */
enum {
  MOD_SHIFT = 1 << 0, MOD_CTRL = 1 << 1, MOD_ALT = 1 << 2, MOD_SUPER = 1 << 3,
  MOD_CAPS = 1 << 4, MOD_NUM = 1 << 5,
};

typedef struct {
  ev_kind_t kind;

  /* EV_KEY */
  int key;            /* GhosttyKey */
  uint16_t mods;      /* MOD_* */
  uint8_t action;     /* KEY_* */
  char text[16];      /* UTF-8 the key produced, if any */
  uint8_t text_len;
  uint32_t unshifted; /* unshifted codepoint, 0 if unknown */

  /* EV_MOUSE */
  uint8_t button;  /* MBTN_* */
  uint8_t maction; /* MOUSE_* */
  uint16_t mx, my; /* 0-indexed cell coordinates */

  /* EV_PASTE */
  const char *paste;
  size_t paste_len;

  /* EV_FOCUS */
  bool focused;
} input_event_t;

typedef struct input_parser input_parser_t;
typedef void (*input_cb_t)(const input_event_t *ev, void *ud);

input_parser_t *input_new(void);
void input_free(input_parser_t *p);

/* Feed bytes; complete events are dispatched to cb. Partial sequences are
 * retained for the next call, so split reads are safe. */
void input_feed(input_parser_t *p, const uint8_t *data, size_t len,
                input_cb_t cb, void *ud);

/* True when a bare ESC (or an unterminated sequence) is buffered. Call
 * input_timeout() after ~50ms of quiet to resolve it as the Escape key. */
bool input_pending(const input_parser_t *p);
void input_timeout(input_parser_t *p, input_cb_t cb, void *ud);

/* Debug/test rendering of an event, e.g. "key ARROW_UP mods=CTRL press". */
void input_event_describe(const input_event_t *ev, char *buf, size_t cap);

#endif /* SL0PPTY_INPUT_H */
