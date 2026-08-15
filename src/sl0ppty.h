/* sl0ppty — common types. See DESIGN.md. */
#ifndef SL0PPTY_H
#define SL0PPTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "input.h"
/* cell_t, color_t and the ATTR_* flags live in the shader ABI, because that is
 * the one place they are a *contract* with code compiled outside this tree. */
#include "shader_abi.h"

/* ---- screen: our composited cell buffer + diff emitter ---------------- */

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

/* ---- paths -------------------------------------------------------------- */

/* Expand a leading `~`, using `buf` when it has to. Returns `path` itself when
 * there is nothing to do, so `buf` only has to outlive the result's use. */
const char *path_expand(const char *path, char *buf, size_t cap);

/* ---- pty --------------------------------------------------------------- */

typedef struct {
  int fd;
  pid_t pid;
} pty_t;

/* `cell_w`/`cell_h` are the client's cell size in pixels, so the pty's
 * winsize carries real pixel dimensions: a program that draws images asks the
 * tty how big a cell is, and zeroes there mean it cannot size one. */
int pty_spawn(pty_t *p, const char *const argv[], uint16_t cols, uint16_t rows,
              const char *cwd, uint16_t cell_w, uint16_t cell_h);
int pty_resize(pty_t *p, uint16_t cols, uint16_t rows, uint16_t cell_w,
               uint16_t cell_h);
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
/* A program in the pane wrote the clipboard (OSC 52). Takes ownership. */
typedef void (*pane_clip_fn)(pane_t *p, char *text, void *ud);
/* A program in the pane sent a notification (OSC 9 / OSC 777). */
typedef void (*pane_notify_fn)(pane_t *p, const char *title, const char *body,
                               void *ud);

pane_t *pane_new(const char *const argv[], uint16_t cols, uint16_t rows,
                 const char *cwd);
/* `suspended`: create the pane but run nothing until pane_start(). `label` is
 * what to show in the meantime (usually the command line). */
pane_t *pane_new_ex(const char *const argv[], uint16_t cols, uint16_t rows,
                    const char *cwd, bool suspended, const char *label);
bool pane_suspended(const pane_t *p);
/* A pane opened to do one thing, rather than one the session is made of: an
 * editor you popped open, not the dev server a layout declared. It runs a
 * command like any other, but when that command ends it is finished — so it
 * keeps no corpse and is not written into a dumped layout. */
void pane_set_ephemeral(pane_t *p, bool yes);
bool pane_ephemeral(const pane_t *p);
const char *pane_label(const pane_t *p);
/* The pid of the program in the pane, or -1. Used to ask the kernel where a
 * shell has got to, which is the only honest answer to "what is this pane's
 * directory" once somebody has typed `cd`. */
pid_t pane_pid(const pane_t *p);
/* Where it was started, which is not where it *is*: see pane_pid(). */
const char *pane_start_cwd(const pane_t *p);
bool pane_start(pane_t *p);
void pane_free(pane_t *p);
int pane_fd(const pane_t *p);
bool pane_alive(const pane_t *p);
/* How the program ended, for a pane that is no longer alive. `code` is the
 * exit status, or the signal number when `signaled`. False while it is still
 * running, and also when the status could not be collected — a pane can be
 * known to be gone without it being known why. */
bool pane_exit(const pane_t *p, int *code, bool *signaled);
/* Run the pane's command again, in the same pty-less pane: same argv, same
 * cwd, same terminal, so the previous run stays above in the scrollback.
 * False if the pane is still alive or the spawn failed. */
bool pane_restart(pane_t *p);
/* Write a line into the pane's own terminal — the backlog, not the pty (which
 * by the time this is wanted is closed). It scrolls with the output it
 * followed, which is the point: it is a thing that happened to this pane. */
void pane_note(pane_t *p, const char *text, color_t fg);
/* Read available pty output into the terminal. Returns bytes read, 0 on EOF,
 * -1 on error (EAGAIN is reported as 0 bytes with alive still true). A pane
 * that reaches EOF stops being alive here and closes its pty, but keeps its
 * terminal: a dead pane is still a pane with contents you can read. */
ssize_t pane_pump(pane_t *p);
void pane_write(pane_t *p, const void *buf, size_t len);
/* Re-encode a decoded event against this pane's own negotiated modes. */
void pane_send_key(pane_t *p, const input_event_t *ev);
void pane_send_mouse(pane_t *p, const input_event_t *ev);
void pane_send_paste(pane_t *p, const char *text, size_t len);
void pane_resize(pane_t *p, uint16_t cols, uint16_t rows);
/* The client's cell size in pixels. Everything about images depends on it:
 * without it a placement that asks for its natural size covers zero cells and
 * is dropped, which is a picture that silently does not appear. */
void pane_set_cell_px(pane_t *p, uint16_t w, uint16_t h);
bool pane_dirty(pane_t *p);
/* Scrollback. Negative delta scrolls up (towards older output). */
void pane_scroll(pane_t *p, int delta);
void pane_scroll_edge(pane_t *p, bool top);
bool pane_scrolled(const pane_t *p);
void pane_scroll_pos(const pane_t *p, uint32_t *above, uint32_t *total);
bool pane_alt_screen(const pane_t *p);
bool pane_wants_mouse(const pane_t *p);
const char *pane_title(const pane_t *p);
/* A BEL arrived and the pane has not been looked at since. */
bool pane_bell(const pane_t *p);
void pane_clear_bell(pane_t *p);
/* The user-assigned name, "" when the program's title is showing through. */
const char *pane_name(const pane_t *p);
/* "" clears the name and gives the pane back to the program's title. */
void pane_set_name(pane_t *p, const char *name);
const char *pane_status(const pane_t *p);
size_t pane_buttons(const pane_t *p, const pane_button_t **out);
void pane_click_button(pane_t *p, const char *id);
void pane_set_osc_handler(pane_t *p, pane_osc_fn fn, void *ud);
void pane_set_clipboard_handler(pane_t *p, pane_clip_fn fn, void *ud);
void pane_set_notify_handler(pane_t *p, pane_notify_fn fn, void *ud);

/* One visible kitty graphics placement, in the pane's viewport coordinates. */
typedef struct {
  uint32_t image_id, place_id;
  uint16_t col, row, cols, rows;
  /* Where the image starts inside its first cell, in pixels. Sub-cell
   * positioning: what a program moving something smoothly relies on, and the
   * difference between motion and a slideshow. */
  uint32_t x_off, y_off;
  /* What the program asked to scale into, or 0 for "draw it at its natural
   * size". The difference matters on the way out: naming a cell count tells
   * the terminal to *scale* the image into it, and the count a natural
   * placement happens to cover changes by one as it slides across a cell
   * boundary — so passing it on makes a moving image change size. */
  uint16_t req_cols, req_rows;
  uint32_t px_w, px_h;    /* rendered size */
  uint32_t src_w, src_h;  /* the image's own size */
  /* The part of the image to draw, in pixels. Clipping has to move this:
   * asking for fewer columns alone *scales* the image into them, it does not
   * crop it. */
  uint32_t sx, sy, sw, sh;
  uint32_t cell_px_w, cell_px_h; /* derived: rendered pixels per cell */
  int format, compression;
  uint64_t generation;
  const uint8_t *data; /* borrowed for the callback only */
  size_t data_len;
} pane_gfx_t;

typedef void (*pane_gfx_fn)(pane_t *p, const pane_gfx_t *g, void *ud);
size_t pane_graphics(pane_t *p, pane_gfx_fn cb, void *ud);

/* Selection, in the pane's viewport coordinates. */
void pane_select_start(pane_t *p, uint16_t x, uint16_t y);
void pane_select_extend(pane_t *p, uint16_t x, uint16_t y);
void pane_select_clear(pane_t *p);
void pane_select_done(pane_t *p);
bool pane_selecting(const pane_t *p);
char *pane_selection_text(pane_t *p);
/* Compose this pane's viewport into the screen at (x0,y0). Clears dirty. */
void pane_compose(pane_t *p, screen_t *s, uint16_t x0, uint16_t y0,
                  bool focused);

#endif /* SL0PPTY_H */
