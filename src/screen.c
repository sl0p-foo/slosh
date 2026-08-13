/* The compositor: a cell buffer for the whole screen, and a diff that turns
 * two of them into the minimal byte stream for the real terminal. */
#define _GNU_SOURCE
#include "sl0ptty.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"

/* ---- hit list ----------------------------------------------------------- */

void hit_reset(hitlist_t *hl) { hl->len = 0; }

void hit_add(hitlist_t *hl, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
             const char *action) {
  if (hl->len == hl->cap) {
    hl->cap = hl->cap ? hl->cap * 2 : 16;
    hl->items = realloc(hl->items, hl->cap * sizeof *hl->items);
  }
  hit_t *e = &hl->items[hl->len++];
  e->x = x;
  e->y = y;
  e->w = w;
  e->h = h;
  snprintf(e->action, sizeof e->action, "%s", action);
}

const char *hit_test(const hitlist_t *hl, uint16_t x, uint16_t y) {
  for (size_t i = hl->len; i-- > 0;) {
    const hit_t *e = &hl->items[i];
    if (x >= e->x && x < e->x + e->w && y >= e->y && y < e->y + e->h)
      return e->action;
  }
  return NULL;
}

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
  free(s->hits.items);
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
  memset(c->text, 0, sizeof c->text);
  memcpy(c->text, txt, len);
  c->len = (uint8_t)len;
  c->width = 1;
  c->fg = fg;
  c->bg = bg;
  c->attrs = attrs;
}

static size_t u8_len(unsigned char b) {
  if (b < 0x80) return 1;
  if ((b & 0xe0) == 0xc0) return 2;
  if ((b & 0xf0) == 0xe0) return 3;
  if ((b & 0xf8) == 0xf0) return 4;
  return 1;
}

uint16_t screen_text(screen_t *s, uint16_t x, uint16_t y, const char *txt,
                     color_t fg, color_t bg, uint16_t attrs) {
  uint16_t n = 0;
  for (const char *p = txt; *p;) {
    size_t len = u8_len((unsigned char)*p);
    if (x + n >= s->cols) break;
    screen_put_utf8(s, (uint16_t)(x + n), y, p, len, fg, bg, attrs);
    p += len;
    n++;
  }
  return n; /* cells written, so a caller can lay out what follows */
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

  /* The cursor is hidden lazily, on the first cell we actually repaint, so a
   * frame with no changes emits nothing at all rather than a hide/show pair.
   * A keystroke that changes nothing should cost zero bytes. */
  bool painted = false;

  if (s->force_full) {
    out_str(s, "\x1b[?25l\x1b[0m\x1b[H\x1b[2J");
    memset(s->prev, 0, (size_t)s->cols * s->rows * sizeof(cell_t));
    painted = true;
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

      if (!painted) {
        out_str(s, "\x1b[?25l");
        painted = true;
      }
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

  bool cursor_moved = s->cursor_visible != s->shown_cursor_visible ||
                      s->cursor_x != s->shown_cursor_x ||
                      s->cursor_y != s->shown_cursor_y;
  if (painted || cursor_moved) {
    if (s->cursor_visible && s->cursor_x < s->cols && s->cursor_y < s->rows) {
      out_fmt(s, "\x1b[%u;%uH", s->cursor_y + 1, s->cursor_x + 1);
      out_str(s, "\x1b[?25h");
    } else if (painted) {
      out_str(s, "\x1b[?25l");
    }
    s->shown_cursor_visible = s->cursor_visible;
    s->shown_cursor_x = s->cursor_x;
    s->shown_cursor_y = s->cursor_y;
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

/* Style runs rather than per-cell objects: a test wants to say "columns 0..5 of
 * row 0 are bold red", and 1920 cell objects per snapshot is unreadable. */
static void dump_style_runs(screen_t *s, json_t *j) {
  json_arr_open(j, "styles");
  for (uint16_t y = 0; y < s->rows; y++) {
    uint16_t x = 0;
    while (x < s->cols) {
      cell_t *c = &s->cur[(size_t)y * s->cols + x];
      bool plain = !c->attrs && !c->fg.set && !c->bg.set;
      if (plain) {
        x++;
        continue;
      }
      uint16_t start = x;
      while (x < s->cols) {
        cell_t *n = &s->cur[(size_t)y * s->cols + x];
        if (n->attrs != c->attrs || !color_eq(n->fg, c->fg) ||
            !color_eq(n->bg, c->bg))
          break;
        x++;
      }
      json_obj_open(j, NULL);
      json_int(j, "x", start);
      json_int(j, "y", y);
      json_int(j, "w", x - start);
      char col[8];
      if (c->fg.set) {
        snprintf(col, sizeof col, "#%02x%02x%02x", c->fg.r, c->fg.g, c->fg.b);
        json_str(j, "fg", col, strlen(col));
      } else {
        json_null(j, "fg");
      }
      if (c->bg.set) {
        snprintf(col, sizeof col, "#%02x%02x%02x", c->bg.r, c->bg.g, c->bg.b);
        json_str(j, "bg", col, strlen(col));
      } else {
        json_null(j, "bg");
      }
      json_arr_open(j, "attrs");
      static const struct {
        uint16_t bit;
        const char *name;
      } names[] = {
          {ATTR_BOLD, "bold"},       {ATTR_DIM, "dim"},
          {ATTR_ITALIC, "italic"},   {ATTR_UNDERLINE, "underline"},
          {ATTR_BLINK, "blink"},     {ATTR_INVERSE, "inverse"},
          {ATTR_INVISIBLE, "invisible"}, {ATTR_STRIKE, "strike"},
      };
      for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (c->attrs & names[i].bit)
          json_str(j, NULL, names[i].name, strlen(names[i].name));
      json_arr_close(j);
      json_obj_close(j);
    }
  }
  json_arr_close(j);
}

char *screen_dump_json(screen_t *s) {
  json_t j;
  json_init(&j);
  json_obj_open(&j, NULL);
  json_int(&j, "cols", s->cols);
  json_int(&j, "rows", s->rows);

  json_arr_open(&j, "text");
  char *line = malloc((size_t)s->cols * 4 + 1);
  for (uint16_t y = 0; y < s->rows; y++) {
    size_t o = 0;
    for (uint16_t x = 0; x < s->cols; x++) {
      cell_t *c = &s->cur[(size_t)y * s->cols + x];
      if (c->width == 0) continue;
      if (c->len) {
        memcpy(line + o, c->text, c->len);
        o += c->len;
      } else {
        line[o++] = ' ';
      }
    }
    json_str(&j, NULL, line, o);
  }
  free(line);
  json_arr_close(&j);

  dump_style_runs(s, &j);

  json_obj_open(&j, "cursor");
  json_bool(&j, "visible", s->cursor_visible);
  json_int(&j, "x", s->cursor_x);
  json_int(&j, "y", s->cursor_y);
  json_obj_close(&j);

  json_arr_open(&j, "hits");
  for (size_t i = 0; i < s->hits.len; i++) {
    hit_t *e = &s->hits.items[i];
    json_obj_open(&j, NULL);
    json_int(&j, "x", e->x);
    json_int(&j, "y", e->y);
    json_int(&j, "w", e->w);
    json_int(&j, "h", e->h);
    json_str(&j, "action", e->action, strlen(e->action));
    json_obj_close(&j);
  }
  json_arr_close(&j);

  json_obj_close(&j);
  return j.buf; /* caller frees */
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
