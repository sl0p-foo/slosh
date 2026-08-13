/* sl0ptty — common types. See DESIGN.md. */
#ifndef SL0PTTY_H
#define SL0PTTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "input.h"

/* ---- screen: our composited cell buffer + diff emitter ---------------- */

enum {
  ATTR_BOLD = 1 << 0,
  ATTR_DIM = 1 << 1,
  ATTR_ITALIC = 1 << 2,
  ATTR_UNDERLINE = 1 << 3,
  ATTR_BLINK = 1 << 4,
  ATTR_INVERSE = 1 << 5,
  ATTR_INVISIBLE = 1 << 6,
  ATTR_STRIKE = 1 << 7,
};

typedef struct {
  bool set; /* false = terminal default */
  uint8_t r, g, b;
} color_t;

/* A composited cell. `len` bytes of UTF-8; a grapheme cluster may be several
 * codepoints, hence the buffer rather than a codepoint. width 0 means this is
 * the tail half of a wide cell and must not be painted. */
typedef struct {
  char text[16];
  uint8_t len;
  uint8_t width;
  uint16_t attrs;
  color_t fg, bg;
} cell_t;

/* One geometry (DESIGN.md): every painted interactive element records its rect
 * here as it is painted, and a click is a lookup. Drawing and hit-testing
 * cannot disagree because there is only one of them. */
typedef struct {
  uint16_t x, y, w, h;
  char action[48];
} hit_t;

typedef struct {
  hit_t *items;
  size_t len, cap;
} hitlist_t;

void hit_reset(hitlist_t *hl);
void hit_add(hitlist_t *hl, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
             const char *action);
/* The action at a point, or NULL. Later entries win: painted last is on top. */
const char *hit_test(const hitlist_t *hl, uint16_t x, uint16_t y);

typedef struct {
  uint16_t cols, rows;
  cell_t *cur;  /* what we want on screen */
  cell_t *prev; /* what the terminal last received */
  bool force_full;
  hitlist_t hits;

  /* where the real cursor goes after a flush */
  bool cursor_visible;
  uint16_t cursor_x, cursor_y;
  /* what the terminal was last told, so an unchanged frame emits nothing */
  bool shown_cursor_visible;
  uint16_t shown_cursor_x, shown_cursor_y;

  /* output accumulator, flushed in one write() */
  char *out;
  size_t out_len, out_cap;
} screen_t;

void screen_init(screen_t *s, uint16_t cols, uint16_t rows);
void screen_free(screen_t *s);
void screen_resize(screen_t *s, uint16_t cols, uint16_t rows);
void screen_clear(screen_t *s);
cell_t *screen_at(screen_t *s, uint16_t x, uint16_t y);
void screen_put_utf8(screen_t *s, uint16_t x, uint16_t y, const char *txt,
                     size_t len, color_t fg, color_t bg, uint16_t attrs);
/* Write a UTF-8 string one cell per codepoint; returns cells written. */
uint16_t screen_text(screen_t *s, uint16_t x, uint16_t y, const char *txt,
                     color_t fg, color_t bg, uint16_t attrs);
/* Diff cur against prev into s->out (the minimal byte stream for a terminal). */
void screen_render(screen_t *s);
/* screen_render, then write it to fd. */
void screen_flush(screen_t *s, int fd);
/* Plain-text dump of the composited screen; caller frees. (headless tests) */
char *screen_dump(screen_t *s);
/* The same screen as JSON: rows of text, style runs, cursor, hit-list. */
char *screen_dump_json(screen_t *s);

/* ---- pty --------------------------------------------------------------- */

typedef struct {
  int fd;
  pid_t pid;
} pty_t;

int pty_spawn(pty_t *p, const char *const argv[], uint16_t cols, uint16_t rows,
              const char *cwd);
int pty_resize(pty_t *p, uint16_t cols, uint16_t rows);
void pty_close(pty_t *p);

/* ---- pane: a pty + a libghostty-vt terminal ---------------------------- */

typedef struct pane pane_t;

/* An OSC 5577 action button drawn in a pane's frame. */
typedef struct {
  char id[33];
  char label[33];
} pane_button_t;

/* Verbs pane.c does not handle itself (purpose, and whatever comes later). */
typedef void (*pane_osc_fn)(pane_t *p, const char *verb, const char *payload,
                            void *ud);

pane_t *pane_new(const char *const argv[], uint16_t cols, uint16_t rows,
                 const char *cwd);
/* `suspended`: create the pane but run nothing until pane_start(). `label` is
 * what to show in the meantime (usually the command line). */
pane_t *pane_new_ex(const char *const argv[], uint16_t cols, uint16_t rows,
                    const char *cwd, bool suspended, const char *label);
bool pane_suspended(const pane_t *p);
const char *pane_label(const pane_t *p);
bool pane_start(pane_t *p);
void pane_free(pane_t *p);
int pane_fd(const pane_t *p);
bool pane_alive(const pane_t *p);
/* Read available pty output into the terminal. Returns bytes read, 0 on EOF,
 * -1 on error (EAGAIN is reported as 0 bytes with alive still true). */
ssize_t pane_pump(pane_t *p);
void pane_write(pane_t *p, const void *buf, size_t len);
/* Re-encode a decoded event against this pane's own negotiated modes. */
void pane_send_key(pane_t *p, const input_event_t *ev);
void pane_send_mouse(pane_t *p, const input_event_t *ev);
void pane_send_paste(pane_t *p, const char *text, size_t len);
void pane_resize(pane_t *p, uint16_t cols, uint16_t rows);
bool pane_dirty(pane_t *p);
/* Scrollback. Negative delta scrolls up (towards older output). */
void pane_scroll(pane_t *p, int delta);
void pane_scroll_edge(pane_t *p, bool top);
bool pane_scrolled(const pane_t *p);
void pane_scroll_pos(const pane_t *p, uint32_t *above, uint32_t *total);
bool pane_alt_screen(const pane_t *p);
bool pane_wants_mouse(const pane_t *p);
const char *pane_title(const pane_t *p);
const char *pane_status(const pane_t *p);
size_t pane_buttons(const pane_t *p, const pane_button_t **out);
void pane_click_button(pane_t *p, const char *id);
void pane_set_osc_handler(pane_t *p, pane_osc_fn fn, void *ud);
/* Compose this pane's viewport into the screen at (x0,y0). Clears dirty. */
void pane_compose(pane_t *p, screen_t *s, uint16_t x0, uint16_t y0,
                  bool focused);

#endif /* SL0PTTY_H */
