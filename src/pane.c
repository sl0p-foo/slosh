/* A pane: one pty, one libghostty-vt terminal, and the code that composites
 * its viewport into the screen. */
#define _GNU_SOURCE
#include "sl0ptty.h"

#include <errno.h>
#include <ghostty/vt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "input.h"

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
};

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

  void *self = p;
  GhosttyTerminalWritePtyFn wfn = on_write_pty;
  GhosttyTerminalTitleChangedFn tfn = on_title_changed;
  ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_USERDATA, &self);
  ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_WRITE_PTY, &wfn);
  ghostty_terminal_set(p->term, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, &tfn);

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
  uint8_t buf[65536];
  ssize_t total = 0;
  for (;;) {
    ssize_t n = read(p->pty.fd, buf, sizeof buf);
    if (n > 0) {
      /* M4: a side-channel scanner for OSC 5577 hooks in here, before the
       * bytes reach the terminal. */
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
  ghostty_mouse_event_set_button(p->mev, (GhosttyMouseButton)ev->button);
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

void pane_resize(pane_t *p, uint16_t cols, uint16_t rows) {
  if (cols == p->cols && rows == p->rows_n) return;
  p->cols = cols;
  p->rows_n = rows;
  ghostty_terminal_resize(p->term, cols, rows, 0, 0);
  pty_resize(&p->pty, cols, rows);
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
