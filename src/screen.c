/* The compositor: a cell buffer for the whole screen, and a diff that turns
 * two of them into the minimal byte stream for the real terminal. */
#define _GNU_SOURCE
#include "sl0ptty.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void out_reserve(screen_t *s, size_t n) {
  if (s->out_len + n <= s->out_cap) return;
  size_t cap = s->out_cap ? s->out_cap : 8192;
  while (cap < s->out_len + n) cap *= 2;
  s->out = realloc(s->out, cap);
  s->out_cap = cap;
}

static void out_bytes(screen_t *s, const char *b, size_t n) {
  out_reserve(s, n);
  memcpy(s->out + s->out_len, b, n);
  s->out_len += n;
}

static void out_str(screen_t *s, const char *b) { out_bytes(s, b, strlen(b)); }

static void out_fmt(screen_t *s, const char *fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) out_bytes(s, buf, (size_t)n);
}

static cell_t blank_cell(void) {
  cell_t c = {0};
  c.text[0] = ' ';
  c.len = 1;
  c.width = 1;
  return c;
}

void screen_init(screen_t *s, uint16_t cols, uint16_t rows) {
  memset(s, 0, sizeof *s);
  s->cols = cols;
  s->rows = rows;
  size_t n = (size_t)cols * rows;
  s->cur = calloc(n, sizeof(cell_t));
  s->prev = calloc(n, sizeof(cell_t));
  s->force_full = true;
  screen_clear(s);
}

void screen_free(screen_t *s) {
  free(s->cur);
  free(s->prev);
  free(s->out);
  memset(s, 0, sizeof *s);
}

void screen_resize(screen_t *s, uint16_t cols, uint16_t rows) {
  if (cols == s->cols && rows == s->rows) return;
  free(s->cur);
  free(s->prev);
  s->cols = cols;
  s->rows = rows;
  size_t n = (size_t)cols * rows;
  s->cur = calloc(n, sizeof(cell_t));
  s->prev = calloc(n, sizeof(cell_t));
  s->force_full = true;
  screen_clear(s);
}

void screen_clear(screen_t *s) {
  size_t n = (size_t)s->cols * s->rows;
  cell_t b = blank_cell();
  for (size_t i = 0; i < n; i++) s->cur[i] = b;
}

cell_t *screen_at(screen_t *s, uint16_t x, uint16_t y) {
  if (x >= s->cols || y >= s->rows) return NULL;
  return &s->cur[(size_t)y * s->cols + x];
}

void screen_put_utf8(screen_t *s, uint16_t x, uint16_t y, const char *txt,
                     size_t len, color_t fg, color_t bg, uint16_t attrs) {
  cell_t *c = screen_at(s, x, y);
  if (!c) return;
  if (len > sizeof c->text) len = sizeof c->text;
  memcpy(c->text, txt, len);
  c->len = (uint8_t)len;
  c->fg = fg;
  c->bg = bg;
  c->attrs = attrs;
}

static bool color_eq(color_t a, color_t b) {
  if (a.set != b.set) return false;
  if (!a.set) return true;
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

static bool cell_eq(const cell_t *a, const cell_t *b) {
  return a->len == b->len && a->width == b->width && a->attrs == b->attrs &&
         memcmp(a->text, b->text, a->len) == 0 && color_eq(a->fg, b->fg) &&
         color_eq(a->bg, b->bg);
}

/* Reset-first SGR: a few bytes more than the minimal encoding, and it cannot
 * inherit a stale attribute. Emitted only when the style actually changes. */
static void emit_sgr(screen_t *s, const cell_t *c) {
  out_str(s, "\x1b[0");
  if (c->attrs & ATTR_BOLD) out_str(s, ";1");
  if (c->attrs & ATTR_DIM) out_str(s, ";2");
  if (c->attrs & ATTR_ITALIC) out_str(s, ";3");
  if (c->attrs & ATTR_UNDERLINE) out_str(s, ";4");
  if (c->attrs & ATTR_BLINK) out_str(s, ";5");
  if (c->attrs & ATTR_INVERSE) out_str(s, ";7");
  if (c->attrs & ATTR_INVISIBLE) out_str(s, ";8");
  if (c->attrs & ATTR_STRIKE) out_str(s, ";9");
  if (c->fg.set) out_fmt(s, ";38;2;%u;%u;%u", c->fg.r, c->fg.g, c->fg.b);
  if (c->bg.set) out_fmt(s, ";48;2;%u;%u;%u", c->bg.r, c->bg.g, c->bg.b);
  out_str(s, "m");
}

static bool style_eq(const cell_t *a, const cell_t *b) {
  return a->attrs == b->attrs && color_eq(a->fg, b->fg) && color_eq(a->bg, b->bg);
}

void screen_flush(screen_t *s, int fd) {
  s->out_len = 0;
  out_str(s, "\x1b[?25l"); /* hide cursor while painting */

  if (s->force_full) {
    out_str(s, "\x1b[0m\x1b[H\x1b[2J");
    memset(s->prev, 0, (size_t)s->cols * s->rows * sizeof(cell_t));
  }

  bool have_pos = false, have_style = false;
  uint16_t cx = 0, cy = 0;
  cell_t style = {0};

  for (uint16_t y = 0; y < s->rows; y++) {
    for (uint16_t x = 0; x < s->cols; x++) {
      cell_t *c = &s->cur[(size_t)y * s->cols + x];
      cell_t *p = &s->prev[(size_t)y * s->cols + x];
      if (c->width == 0) continue; /* tail of a wide cell: never painted */
      if (cell_eq(c, p)) continue;

      if (!have_pos || cx != x || cy != y) {
        out_fmt(s, "\x1b[%u;%uH", y + 1, x + 1);
        cx = x;
        cy = y;
        have_pos = true;
      }
      if (!have_style || !style_eq(c, &style)) {
        emit_sgr(s, c);
        style = *c;
        have_style = true;
      }
      if (c->len) out_bytes(s, c->text, c->len);
      else out_str(s, " ");

      *p = *c;
      cx += c->width ? c->width : 1;
      if (cx >= s->cols) have_pos = false; /* wrap is terminal-dependent */
    }
  }

  if (s->cursor_visible && s->cursor_x < s->cols && s->cursor_y < s->rows) {
    out_fmt(s, "\x1b[%u;%uH", s->cursor_y + 1, s->cursor_x + 1);
    out_str(s, "\x1b[?25h");
  }

  s->force_full = false;
  if (s->out_len) {
    size_t off = 0;
    while (off < s->out_len) {
      ssize_t n = write(fd, s->out + off, s->out_len - off);
      if (n <= 0) break;
      off += (size_t)n;
    }
  }
}

char *screen_dump(screen_t *s) {
  size_t cap = (size_t)(s->cols + 1) * s->rows * 4 + 1;
  char *buf = malloc(cap);
  size_t o = 0;
  for (uint16_t y = 0; y < s->rows; y++) {
    for (uint16_t x = 0; x < s->cols; x++) {
      cell_t *c = &s->cur[(size_t)y * s->cols + x];
      if (c->width == 0) continue;
      if (c->len) {
        memcpy(buf + o, c->text, c->len);
        o += c->len;
      } else {
        buf[o++] = ' ';
      }
    }
    buf[o++] = '\n';
  }
  buf[o] = 0;
  return buf;
}
