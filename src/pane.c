/* A pane: one pty, one libghostty-vt terminal, and the code that composites
 * its viewport into the screen. */
#define _GNU_SOURCE
#include "sl0ptty.h"

#include <errno.h>
#include <stdio.h>
#include <ghostty/vt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "input.h"
#include "osc5577.h"

struct pane {
  pty_t pty;
  GhosttyTerminal term;
  GhosttyRenderState rstate;
  GhosttyRenderStateRowIterator rows;
  GhosttyRenderStateRowCells cells;
  GhosttyKeyEncoder kenc;
  GhosttyKeyEvent kev;
  GhosttyMouseEncoder menc;
  GhosttyMouseEvent mev;
  uint16_t cols, rows_n;
  bool alive;
  bool dirty;
  char title[256];

  /* OSC 5577 state: what this pane asked us to draw in its frame */
  /* A suspended pane is real, sized and laid out, but has not run anything:
   * a root with twelve projects must not become twelve running servers. It
   * starts on the first keystroke it is given. */
  bool suspended;
  char **argv;
  char *cwd;
  char label[128];

  /* Selection anchor, in viewport coordinates, while a drag is in progress. */
  bool selecting;
  uint16_t sel_x, sel_y;

  osc_scan_t scan;
  char status[256];
  pane_button_t buttons[8];
  size_t nbuttons;
  pane_osc_fn osc_cb;
  void *osc_ud;
  pane_clip_fn clip_cb;
  void *clip_ud;
  pane_notify_fn notify_cb;
  void *notify_ud;
};

void pane_set_osc_handler(pane_t *p, pane_osc_fn fn, void *ud) {
  p->osc_cb = fn;
  p->osc_ud = ud;
}

const char *pane_status(const pane_t *p) { return p->status; }
size_t pane_buttons(const pane_t *p, const pane_button_t **out) {
  *out = p->buttons;
  return p->nbuttons;
}

void pane_click_button(pane_t *p, const char *id) {
  if (!osc5577_valid_id(id)) return;
  char msg[64];
  int n = snprintf(msg, sizeof msg, "\033]5577;1;click;%s\033\\", id);
  pane_write(p, msg, (size_t)n);
}

/* Buttons are `id:label` fields, each %-escaped because the payload itself is
 * ;-separated. An invalid id or an empty label drops that button and only
 * that button. */
static void parse_buttons(pane_t *p, const char *payload) {
  p->nbuttons = 0;
  const char *cur = payload;
  while (*cur && p->nbuttons < sizeof p->buttons / sizeof *p->buttons) {
    const char *semi = strchr(cur, ';');
    size_t flen = semi ? (size_t)(semi - cur) : strlen(cur);
    const char *colon = memchr(cur, ':', flen);
    if (colon) {
      pane_button_t b = {0};
      /* Unescape the id into a buffer far larger than the limit, so an
       * over-long id is *rejected* rather than truncated into a valid one.
       * Truncating here would let `aaa...a<33 chars>` collide with a
       * legitimate 32-char id that the program already trusts. */
      char id[256];
      osc5577_unescape(cur, (size_t)(colon - cur), id, sizeof id);
      osc5577_unescape(colon + 1, flen - (size_t)(colon - cur) - 1, b.label,
                       sizeof b.label);
      if (osc5577_valid_id(id) && b.label[0]) {
        snprintf(b.id, sizeof b.id, "%s", id);
        p->buttons[p->nbuttons++] = b;
      }
    }
    if (!semi) break;
    cur = semi + 1;
  }
  p->dirty = true;
}

static void on_osc5577(const char *verb, const char *payload, void *ud) {
  pane_t *p = ud;
  if (strcmp(verb, "status") == 0) {
    snprintf(p->status, sizeof p->status, "%s", payload);
    p->dirty = true;
  } else if (strcmp(verb, "buttons") == 0) {
    parse_buttons(p, payload);
  } else if (strcmp(verb, "clear") == 0) {
    p->status[0] = 0;
    p->nbuttons = 0;
    p->dirty = true;
  } else if (strcmp(verb, "hello") == 0) {
    /* Our addition to the fork's protocol, which is unversioned in practice:
     * a program can ask what it is talking to before using anything new. */
    const char *reply = "\033]5577;1;hello;sl0ptty;1\033\\";
    pane_write(p, reply, strlen(reply));
  } else if (p->osc_cb) {
    p->osc_cb(p, verb, payload, p->osc_ud); /* purpose, and anything later */
  }
}

/* Terminal replies (DA, cursor position, XTVERSION...) go back to the app. */
static void on_write_pty(GhosttyTerminal t, void *ud, const uint8_t *data,
                         size_t len) {
  pane_t *p = ud;
  if (!p || p->pty.fd < 0) return;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(p->pty.fd, data + off, len - off);
    if (n <= 0) break;
    off += (size_t)n;
  }
}

/* A program in the pane wrote the clipboard (OSC 52 / OSC 1337). lib-vt has
 * already normalised the base64, the chunking and the selectors. */
static GhosttyClipboardWriteResult on_clipboard_write(
    GhosttyTerminal t, void *ud, const GhosttyClipboardWrite *write) {
  pane_t *p = ud;
  if (!p->clip_cb || !write || !write->contents_len)
    return GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
  const GhosttyString *data = &write->contents[0].data;
  if (!data->len) return GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
  char *copy = malloc(data->len + 1);
  memcpy(copy, data->ptr, data->len);
  copy[data->len] = 0;
  p->clip_cb(p, copy, p->clip_ud); /* the handler takes ownership */
  return GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
}

void pane_set_clipboard_handler(pane_t *p, pane_clip_fn fn, void *ud) {
  p->clip_cb = fn;
  p->clip_ud = ud;
}

void pane_set_notify_handler(pane_t *p, pane_notify_fn fn, void *ud) {
  p->notify_cb = fn;
  p->notify_ud = ud;
}

/* OSC 9 / OSC 777: a program in the pane wants to say something. */
static void on_notify(GhosttyTerminal t, void *ud,
                      const GhosttyTerminalDesktopNotification *n) {
  pane_t *p = ud;
  if (!p->notify_cb || !n) return;
  char title[96] = {0}, body[96] = {0};
  if (n->title.len)
    snprintf(title, sizeof title, "%.*s", (int)n->title.len, (const char *)n->title.ptr);
  if (n->body.len)
    snprintf(body, sizeof body, "%.*s", (int)n->body.len, (const char *)n->body.ptr);
  p->notify_cb(p, title, body, p->notify_ud);
}

static void on_title_changed(GhosttyTerminal t, void *ud) {
  pane_t *p = ud;
  GhosttyString s = {0};
  if (ghostty_terminal_get(p->term, GHOSTTY_TERMINAL_DATA_TITLE, &s) !=
      GHOSTTY_SUCCESS)
    return;
  size_t n = s.len < sizeof p->title - 1 ? s.len : sizeof p->title - 1;
  memcpy(p->title, s.ptr, n);
  p->title[n] = 0;
  p->dirty = true;
}

bool pane_suspended(const pane_t *p) { return p->suspended; }
const char *pane_label(const pane_t *p) { return p->label; }

/* Spawn what a suspended pane was created to run. */
bool pane_start(pane_t *p) {
  if (!p->suspended) return false;
  p->suspended = false;
  if (pty_spawn(&p->pty, (const char *const *)p->argv, p->cols, p->rows_n,
                p->cwd) != 0) {
    p->alive = false;
    return false;
  }
  p->alive = true;
  p->dirty = true;
  return true;
}

static char **argv_dup(const char *const argv[]) {
  size_t n = 0;
  while (argv[n]) n++;
  char **out = calloc(n + 1, sizeof *out);
  for (size_t i = 0; i < n; i++) out[i] = strdup(argv[i]);
  return out;
}

pane_t *pane_new_ex(const char *const argv[], uint16_t cols, uint16_t rows,
                    const char *cwd, bool suspended, const char *label) {
  pane_t *p = pane_new(argv, cols, rows, cwd);
  if (!p) return NULL;
  snprintf(p->label, sizeof p->label, "%s", label ? label : "");
  if (suspended) {
    /* pane_new already spawned; a suspended pane must not have. Close it and
     * keep the arguments for later, rather than duplicating the constructor. */
    pty_close(&p->pty);
    p->pty.fd = -1;
    p->suspended = true;
    p->alive = true;
    p->argv = argv_dup(argv);
    p->cwd = cwd ? strdup(cwd) : NULL;
  }
  return p;
}

pane_t *pane_new(const char *const argv[], uint16_t cols, uint16_t rows,
                 const char *cwd) {
  pane_t *p = calloc(1, sizeof *p);
  p->cols = cols;
  p->rows_n = rows;
  p->pty.fd = -1;

  if (ghostty_terminal_new(NULL, &p->term, cols, rows) != GHOSTTY_SUCCESS)
    goto fail;
  if (ghostty_render_state_new(NULL, &p->rstate) != GHOSTTY_SUCCESS) goto fail;
  if (ghostty_render_state_row_iterator_new(NULL, &p->rows) != GHOSTTY_SUCCESS)
    goto fail;
  if (ghostty_render_state_row_cells_new(NULL, &p->cells) != GHOSTTY_SUCCESS)
    goto fail;
  if (ghostty_key_encoder_new(NULL, &p->kenc) != GHOSTTY_SUCCESS) goto fail;
  if (ghostty_key_event_new(NULL, &p->kev) != GHOSTTY_SUCCESS) goto fail;
  if (ghostty_mouse_encoder_new(NULL, &p->menc) != GHOSTTY_SUCCESS) goto fail;
  if (ghostty_mouse_event_new(NULL, &p->mev) != GHOSTTY_SUCCESS) goto fail;

  /* Careful: for pointer-typed options the header says "Input type:
   * GhosttyTerminalWritePtyFn", not "...Fn *" — the value pointer IS the
   * value. Passing &fn stores the address of a stack local as the callback,
   * which survives exactly until pane_new() returns and then jumps into a
   * dead frame. Struct-typed options (MODE) do take a pointer; the rule is
   * that a pointer-shaped value is passed as itself. */
  ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_USERDATA, p);
  ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                       (const void *)(uintptr_t)on_write_pty);
  ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                       (const void *)(uintptr_t)on_title_changed);
  ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,
                       (const void *)(uintptr_t)on_clipboard_write);
  ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION,
                       (const void *)(uintptr_t)on_notify);

  /* lib-vt makes no assumptions a host terminal would: it starts with DECTCEM
   * (mode 25) off, so the cursor is invisible until someone says otherwise.
   * MODE_DEFAULT sets the current value *and* the one restored by RIS, so a
   * program that resets the terminal does not lose its cursor.
   *
   * 2027 is grapheme clustering: one cell per cluster, so flags and ZWJ emoji
   * stay whole. We are opinionated about the outer terminal (D11) and it
   * clusters too; under a terminal that does not, wide emoji may misalign. */
  static const uint16_t default_on[] = {25, 2027};
  for (size_t i = 0; i < sizeof default_on / sizeof *default_on; i++) {
    GhosttyTerminalModeConfig mc = {.mode = ghostty_mode_new(default_on[i], false),
                                    .value = true};
    ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_MODE_DEFAULT, &mc);
  }

  if (pty_spawn(&p->pty, argv, cols, rows, cwd) != 0) goto fail;
  p->alive = true;
  p->dirty = true;
  return p;

fail:
  pane_free(p);
  return NULL;
}

void pane_free(pane_t *p) {
  if (!p) return;
  if (p->argv) {
    for (size_t i = 0; p->argv[i]; i++) free(p->argv[i]);
    free(p->argv);
  }
  free(p->cwd);
  if (p->pty.fd >= 0) pty_close(&p->pty);
  if (p->mev) ghostty_mouse_event_free(p->mev);
  if (p->menc) ghostty_mouse_encoder_free(p->menc);
  if (p->kev) ghostty_key_event_free(p->kev);
  if (p->kenc) ghostty_key_encoder_free(p->kenc);
  if (p->cells) ghostty_render_state_row_cells_free(p->cells);
  if (p->rows) ghostty_render_state_row_iterator_free(p->rows);
  if (p->rstate) ghostty_render_state_free(p->rstate);
  if (p->term) ghostty_terminal_free(p->term);
  free(p);
}

int pane_fd(const pane_t *p) { return p->pty.fd; }
bool pane_alive(const pane_t *p) { return p->alive; }
bool pane_dirty(pane_t *p) { return p->dirty; }
const char *pane_title(const pane_t *p) { return p->title; }

ssize_t pane_pump(pane_t *p) {
  if (p->pty.fd < 0) return 0;
  uint8_t buf[65536];
  ssize_t total = 0;
  for (;;) {
    ssize_t n = read(p->pty.fd, buf, sizeof buf);
    if (n > 0) {
      /* The scanner sees the same bytes the terminal does. lib-vt discards an
       * OSC it does not know, so nothing is drawn and nothing is buffered. */
      osc_scan_feed(&p->scan, buf, (size_t)n, on_osc5577, p);
      ghostty_terminal_vt_write(p->term, buf, (size_t)n);
      p->dirty = true;
      total += n;
      if ((size_t)n < sizeof buf) break; /* drained */
      continue;
    }
    if (n == 0) {
      p->alive = false;
      return 0;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    if (errno == EINTR) continue;
    p->alive = false;
    return -1;
  }
  return total;
}

void pane_write(pane_t *p, const void *buf, size_t len) {
  if (p->pty.fd < 0) return;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(p->pty.fd, (const char *)buf + off, len - off);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      break;
    }
    off += (size_t)n;
  }
}

/* Re-encode a decoded event for *this* pane, against the modes this pane has
 * actually negotiated (kitty flags, cursor-key application mode, alt-esc
 * prefix, modifyOtherKeys). This is the reason we decode at all. */
void pane_send_key(pane_t *p, const input_event_t *ev) {
  /* Typing snaps back to the live view: reading scrollback and then typing
   * into a screen that is not the one you are looking at is a trap. */
  if (pane_scrolled(p)) pane_scroll_edge(p, false);
  ghostty_key_encoder_setopt_from_terminal(p->kenc, p->term);
  ghostty_key_event_set_key(p->kev, (GhosttyKey)ev->key);
  ghostty_key_event_set_mods(p->kev, (GhosttyMods)ev->mods);
  ghostty_key_event_set_action(p->kev, (GhosttyKeyAction)ev->action);
  ghostty_key_event_set_utf8(p->kev, ev->text_len ? ev->text : NULL,
                             ev->text_len);
  ghostty_key_event_set_unshifted_codepoint(p->kev, ev->unshifted);

  char out[128];
  size_t n = 0;
  if (ghostty_key_encoder_encode(p->kenc, p->kev, out, sizeof out, &n) ==
          GHOSTTY_SUCCESS &&
      n > 0)
    pane_write(p, out, n);
}

void pane_send_mouse(pane_t *p, const input_event_t *ev) {
  ghostty_mouse_encoder_setopt_from_terminal(p->menc, p->term);

  /* The encoder works in surface pixels. We have no pixels, so we declare a
   * 1x1-pixel cell: cell coordinates and "pixels" become the same number, and
   * SGR-pixel mode degrades to cell precision instead of lying. */
  GhosttyMouseEncoderSize size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
  size.screen_width = p->cols;
  size.screen_height = p->rows_n;
  size.cell_width = 1;
  size.cell_height = 1;
  ghostty_mouse_encoder_setopt(p->menc, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);

  GhosttyMousePosition pos = {.x = (float)ev->mx, .y = (float)ev->my};
  ghostty_mouse_event_set_position(p->mev, pos);
  /* "no button held" is its own thing, not button zero: bare motion (hover)
   * must be encoded as none, or a pane in any-event tracking sees nothing. */
  if (ev->button == MBTN_UNKNOWN) ghostty_mouse_event_clear_button(p->mev);
  else ghostty_mouse_event_set_button(p->mev, (GhosttyMouseButton)ev->button);
  ghostty_mouse_event_set_action(p->mev, (GhosttyMouseAction)ev->maction);
  ghostty_mouse_event_set_mods(p->mev, (GhosttyMods)ev->mods);

  char out[128];
  size_t n = 0;
  if (ghostty_mouse_encoder_encode(p->menc, p->mev, out, sizeof out, &n) ==
          GHOSTTY_SUCCESS &&
      n > 0)
    pane_write(p, out, n);
}

/* Bracketed-paste aware, and lib-vt decides whether the payload is safe. */
void pane_send_paste(pane_t *p, const char *text, size_t len) {
  GhosttyTerminalModeConfig mc = {.mode = ghostty_mode_new(2004, false)};
  ghostty_terminal_get(p->term, GHOSTTY_TERMINAL_DATA_MODE, &mc);
  bool bracketed = mc.value;

  size_t need = 0;
  ghostty_paste_encode((char *)text, len, bracketed, NULL, 0, &need);
  char *buf = malloc(need ? need : len + 16);
  size_t n = 0;
  if (ghostty_paste_encode((char *)text, len, bracketed, buf,
                           need ? need : len + 16, &n) == GHOSTTY_SUCCESS)
    pane_write(p, buf, n);
  free(buf);
}

/* ---- selection ---------------------------------------------------------- *
 *
 * A selection is two grid references and a flag, so it can be built directly
 * from two viewport positions — no gesture machinery, and the render state
 * reports the per-row range for us to highlight.
 */

static bool grid_ref_at(pane_t *p, uint16_t x, uint16_t y, GhosttyGridRef *out) {
  GhosttyPoint pt = {.tag = GHOSTTY_POINT_TAG_VIEWPORT,
                     .value.coordinate = {.x = x, .y = y}};
  return ghostty_terminal_grid_ref(p->term, pt, out) == GHOSTTY_SUCCESS;
}

void pane_select_start(pane_t *p, uint16_t x, uint16_t y) {
  p->selecting = true;
  p->sel_x = x;
  p->sel_y = y;
  pane_select_clear(p);
}

void pane_select_extend(pane_t *p, uint16_t x, uint16_t y) {
  if (!p->selecting) return;
  GhosttyGridRef a, b;
  if (!grid_ref_at(p, p->sel_x, p->sel_y, &a) || !grid_ref_at(p, x, y, &b))
    return;
  GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
  sel.start = a;
  sel.end = b;
  sel.rectangle = false;
  ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_SELECTION, &sel);
  p->dirty = true;
}

void pane_select_clear(pane_t *p) {
  ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_SELECTION, NULL);
  p->dirty = true;
}

bool pane_selecting(const pane_t *p) { return p->selecting; }
void pane_select_done(pane_t *p) { p->selecting = false; }

/* The selected text, or NULL. Caller frees. */
char *pane_selection_text(pane_t *p) {
  GhosttyTerminalSelectionFormatOptions opts =
      GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
  opts.trim = true;
  uint8_t *ptr = NULL;
  size_t len = 0;
  if (ghostty_terminal_selection_format_alloc(p->term, NULL, opts, &ptr, &len) !=
          GHOSTTY_SUCCESS ||
      !len)
    return NULL;
  char *out = malloc(len + 1);
  memcpy(out, ptr, len);
  out[len] = 0;
  ghostty_free(NULL, ptr, len);
  return out;
}

/* ---- scrollback --------------------------------------------------------- */

void pane_scroll(pane_t *p, int delta) {
  GhosttyTerminalScrollViewport b = {.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA};
  b.value.delta = delta;
  ghostty_terminal_scroll_viewport(p->term, b);
  p->dirty = true;
}

void pane_scroll_edge(pane_t *p, bool top) {
  GhosttyTerminalScrollViewport b = {
      .tag = top ? GHOSTTY_SCROLL_VIEWPORT_TOP : GHOSTTY_SCROLL_VIEWPORT_BOTTOM};
  ghostty_terminal_scroll_viewport(p->term, b);
  p->dirty = true;
}

bool pane_scrolled(const pane_t *p) {
  bool active = true; /* "the viewport is on the active area", i.e. the bottom */
  ghostty_terminal_get(p->term, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &active);
  return !active;
}

/* Rows hidden above the viewport, and how many exist in total. */
void pane_scroll_pos(const pane_t *p, uint32_t *above, uint32_t *total) {
  GhosttyTerminalScrollbar sb = {0};
  *above = *total = 0;
  if (ghostty_terminal_get(p->term, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &sb) !=
      GHOSTTY_SUCCESS)
    return;
  *above = (uint32_t)sb.offset;
  *total = (uint32_t)(sb.total > sb.len ? sb.total - sb.len : 0);
}

bool pane_alt_screen(const pane_t *p) {
  GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
  ghostty_terminal_get(p->term, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
  return screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
}

bool pane_wants_mouse(const pane_t *p) {
  GhosttyMouseTrackingMode mode = GHOSTTY_MOUSE_TRACKING_NONE;
  ghostty_terminal_get(p->term, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &mode);
  return mode != GHOSTTY_MOUSE_TRACKING_NONE;
}

void pane_resize(pane_t *p, uint16_t cols, uint16_t rows) {
  if (cols == p->cols && rows == p->rows_n) return;
  p->cols = cols;
  p->rows_n = rows;
  ghostty_terminal_resize(p->term, cols, rows, 0, 0);
  if (p->pty.fd >= 0) pty_resize(&p->pty, cols, rows);
  p->dirty = true;
}

static color_t to_color(const GhosttyColorRgb *c, bool ok) {
  color_t out = {0};
  if (ok) {
    out.set = true;
    out.r = c->r;
    out.g = c->g;
    out.b = c->b;
  }
  return out;
}

void pane_compose(pane_t *p, screen_t *s, uint16_t x0, uint16_t y0,
                  bool focused) {
  if (ghostty_render_state_update(p->rstate, p->term) != GHOSTTY_SUCCESS) return;

  if (ghostty_render_state_get(p->rstate, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                               &p->rows) != GHOSTTY_SUCCESS)
    return;

  uint16_t y = 0;
  while (ghostty_render_state_row_iterator_next(p->rows)) {
    if (y >= p->rows_n) break;
    if (ghostty_render_state_row_get(p->rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &p->cells) != GHOSTTY_SUCCESS) {
      y++;
      continue;
    }

    GhosttyRenderStateRowSelection rowsel =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
    bool has_sel = ghostty_render_state_row_get(
                       p->rows, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION,
                       &rowsel) == GHOSTTY_SUCCESS;

    uint16_t x = 0;
    while (ghostty_render_state_row_cells_next(p->cells)) {
      if (x >= p->cols) break;
      cell_t *dst = screen_at(s, (uint16_t)(x0 + x), (uint16_t)(y0 + y));
      if (!dst) break;

      GhosttyCell raw = {0};
      GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
      if (ghostty_render_state_row_cells_get(
              p->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw) ==
          GHOSTTY_SUCCESS)
        ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);

      if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
        dst->width = 0;
        dst->len = 0;
        x++;
        continue;
      }

      char utf8[16] = {0};
      GhosttyBuffer gb = {.ptr = (uint8_t *)utf8, .cap = sizeof utf8, .len = 0};
      if (ghostty_render_state_row_cells_get(
              p->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
              &gb) != GHOSTTY_SUCCESS)
        gb.len = 0;

      GhosttyColorRgb fg = {0}, bg = {0};
      bool have_fg = ghostty_render_state_row_cells_get(
                         p->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                         &fg) == GHOSTTY_SUCCESS;
      bool have_bg = ghostty_render_state_row_cells_get(
                         p->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                         &bg) == GHOSTTY_SUCCESS;

      uint16_t attrs = 0;
      bool styled = false;
      if (ghostty_render_state_row_cells_get(
              p->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_HAS_STYLING,
              &styled) == GHOSTTY_SUCCESS &&
          styled) {
        GhosttyStyle st = GHOSTTY_INIT_SIZED(GhosttyStyle);
        if (ghostty_render_state_row_cells_get(
                p->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &st) ==
            GHOSTTY_SUCCESS) {
          if (st.bold) attrs |= ATTR_BOLD;
          if (st.faint) attrs |= ATTR_DIM;
          if (st.italic) attrs |= ATTR_ITALIC;
          if (st.underline) attrs |= ATTR_UNDERLINE;
          if (st.blink) attrs |= ATTR_BLINK;
          if (st.inverse) attrs |= ATTR_INVERSE;
          if (st.invisible) attrs |= ATTR_INVISIBLE;
          if (st.strikethrough) attrs |= ATTR_STRIKE;
        }
      }

      size_t n = gb.len < sizeof dst->text ? gb.len : sizeof dst->text;
      memset(dst->text, 0, sizeof dst->text);
      if (n) memcpy(dst->text, utf8, n);
      else { dst->text[0] = ' '; n = 1; }
      dst->len = (uint8_t)n;
      dst->width = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1;
      /* Selected cells are inverted rather than recoloured, so a selection
       * reads the same over any theme the program inside is using. */
      if (has_sel && x >= rowsel.start_x && x <= rowsel.end_x)
        attrs ^= ATTR_INVERSE;

      dst->attrs = attrs;
      dst->fg = to_color(&fg, have_fg);
      dst->bg = to_color(&bg, have_bg);
      x++;
    }
    y++;
  }

  if (focused) {
    bool vis = false, has_pos = false;
    uint16_t cxp = 0, cyp = 0;
    ghostty_render_state_get(p->rstate, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE,
                             &vis);
    ghostty_render_state_get(
        p->rstate, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &has_pos);
    if (vis && has_pos) {
      ghostty_render_state_get(p->rstate,
                               GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cxp);
      ghostty_render_state_get(p->rstate,
                               GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cyp);
      s->cursor_visible = true;
      s->cursor_x = (uint16_t)(x0 + cxp);
      s->cursor_y = (uint16_t)(y0 + cyp);
    } else {
      s->cursor_visible = false;
    }
  }

  GhosttyRenderStateDirty clean = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
  ghostty_render_state_set(p->rstate, GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean);
  p->dirty = false;
}
