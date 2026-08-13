#define _GNU_SOURCE
#include "graphics.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Our ids live high, so they cannot collide with images the client's terminal
 * already knows about from before we attached. */
#define OUT_ID_BASE 0x51000000u
#define CHUNK 3072 /* bytes of base64 per escape, kitty's documented ceiling */

struct graphics {
  gfx_image_t *imgs;
  size_t nimgs, imgcap;
  gfx_place_t *places;
  size_t nplaces, placecap;
  uint32_t next_out_id;

  char *out;
  size_t len, cap;
};

static void out_reserve(graphics_t *g, size_t n) {
  if (g->len + n + 1 <= g->cap) return;
  size_t cap = g->cap ? g->cap : 8192;
  while (cap < g->len + n + 1) cap *= 2;
  g->out = realloc(g->out, cap);
  g->cap = cap;
}

static void out_str(graphics_t *g, const char *s) {
  size_t n = strlen(s);
  out_reserve(g, n);
  memcpy(g->out + g->len, s, n);
  g->len += n;
  g->out[g->len] = 0;
}

static void out_fmt(graphics_t *g, const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) out_str(g, buf);
}

static void out_b64(graphics_t *g, const uint8_t *in, size_t len) {
  static const char T[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out_reserve(g, ((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    unsigned v = (unsigned)in[i] << 16;
    if (i + 1 < len) v |= (unsigned)in[i + 1] << 8;
    if (i + 2 < len) v |= in[i + 2];
    g->out[g->len++] = T[(v >> 18) & 63];
    g->out[g->len++] = T[(v >> 12) & 63];
    g->out[g->len++] = i + 1 < len ? T[(v >> 6) & 63] : '=';
    g->out[g->len++] = i + 2 < len ? T[v & 63] : '=';
  }
  g->out[g->len] = 0;
}

graphics_t *gfx_new(void) {
  graphics_t *g = calloc(1, sizeof *g);
  g->next_out_id = OUT_ID_BASE;
  return g;
}

void gfx_free(graphics_t *g) {
  if (!g) return;
  free(g->imgs);
  free(g->places);
  free(g->out);
  free(g);
}

void gfx_reset(graphics_t *g) {
  for (size_t i = 0; i < g->nimgs; i++) g->imgs[i].sent = false;
  g->nplaces = 0;
}

void gfx_forget_pane(graphics_t *g, uint32_t pane) {
  size_t keep = 0;
  for (size_t i = 0; i < g->nimgs; i++)
    if (g->imgs[i].pane != pane) g->imgs[keep++] = g->imgs[i];
  g->nimgs = keep;
}

void gfx_begin(graphics_t *g) {
  for (size_t i = 0; i < g->nplaces; i++) g->places[i].live = false;
  g->len = 0;
  if (g->out) g->out[0] = 0;
}

static gfx_image_t *img_find(graphics_t *g, uint32_t pane, uint32_t src_id) {
  for (size_t i = 0; i < g->nimgs; i++)
    if (g->imgs[i].pane == pane && g->imgs[i].src_id == src_id)
      return &g->imgs[i];
  return NULL;
}

/* a=t: transmit without placing, so the same bytes can be placed repeatedly */
static void transmit(graphics_t *g, gfx_image_t *img, uint32_t px_w,
                     uint32_t px_h, int format, int compression,
                     const uint8_t *data, size_t len) {
  if (!data || !len) return;
  int f = format == 0 ? 24 : format == 1 ? 32 : format == 2 ? 100 : 32;

  size_t raw_chunk = (CHUNK / 4) * 3;
  for (size_t off = 0; off < len; off += raw_chunk) {
    size_t n = len - off < raw_chunk ? len - off : raw_chunk;
    bool more = off + n < len;
    out_str(g, "\x1b_G");
    if (off == 0) {
      out_fmt(g, "a=t,q=2,i=%u,f=%d,s=%u,v=%u", img->out_id, f, px_w, px_h);
      if (compression == 1) out_str(g, ",o=z");
      out_fmt(g, ",m=%d;", more ? 1 : 0);
    } else {
      out_fmt(g, "m=%d;", more ? 1 : 0);
    }
    out_b64(g, data + off, n);
    out_str(g, "\x1b\\");
  }
  img->sent = true;
}

void gfx_place(graphics_t *g, uint32_t pane, uint32_t src_id, uint64_t gen,
               uint32_t place_id, uint16_t col, uint16_t row, uint16_t cols,
               uint16_t rows, uint32_t sx, uint32_t sy, uint32_t sw,
               uint32_t sh, uint32_t px_w, uint32_t px_h, int format,
               int compression, const uint8_t *data, size_t data_len) {
  gfx_image_t *img = img_find(g, pane, src_id);
  if (!img) {
    if (g->nimgs == g->imgcap) {
      g->imgcap = g->imgcap ? g->imgcap * 2 : 8;
      g->imgs = realloc(g->imgs, g->imgcap * sizeof *g->imgs);
    }
    img = &g->imgs[g->nimgs++];
    *img = (gfx_image_t){
        .pane = pane, .src_id = src_id, .out_id = g->next_out_id++, .gen = gen};
  }
  if (img->gen != gen) { /* the program replaced the image under the same id */
    img->gen = gen;
    img->sent = false;
  }
  if (!img->sent)
    transmit(g, img, px_w, px_h, format, compression, data, data_len);

  uint32_t pid = place_id ? place_id : 1;
  gfx_place_t *slot = NULL;
  for (size_t i = 0; i < g->nplaces; i++)
    if (g->places[i].out_id == img->out_id && g->places[i].place_id == pid)
      slot = &g->places[i];
  if (!slot) {
    if (g->nplaces == g->placecap) {
      g->placecap = g->placecap ? g->placecap * 2 : 8;
      g->places = realloc(g->places, g->placecap * sizeof *g->places);
    }
    slot = &g->places[g->nplaces++];
    memset(slot, 0, sizeof *slot);
    slot->out_id = img->out_id;
    slot->place_id = pid;
  }
  slot->col = col;
  slot->row = row;
  slot->cols = cols;
  slot->rows = rows;
  slot->sx = sx;
  slot->sy = sy;
  slot->sw = sw;
  slot->sh = sh;
  slot->live = true;
}

char *gfx_flush(graphics_t *g, size_t *out_len) {
  /* Placements that were on screen last frame and are not now must be told to
   * go away; a terminal will happily keep drawing an image nobody owns. */
  size_t keep = 0;
  for (size_t i = 0; i < g->nplaces; i++) {
    gfx_place_t *p = &g->places[i];
    if (p->live) {
      g->places[keep++] = *p;
      continue;
    }
    out_fmt(g, "\x1b_Ga=d,d=i,q=2,i=%u,p=%u\x1b\\", p->out_id, p->place_id);
  }
  g->nplaces = keep;

  /* Then (re)place everything that is. C=1 keeps the cursor where the text
   * renderer left it, which matters because we are interleaving with a diff. */
  for (size_t i = 0; i < g->nplaces; i++) {
    gfx_place_t *p = &g->places[i];
    out_fmt(g, "\x1b[%u;%uH", p->row + 1, p->col + 1);
    out_fmt(g, "\x1b_Ga=p,q=2,C=1,i=%u,p=%u,c=%u,r=%u", p->out_id, p->place_id,
            p->cols, p->rows);
    /* The source rectangle is what actually crops; c/r alone would scale. */
    if (p->sw || p->sh)
      out_fmt(g, ",x=%u,y=%u,w=%u,h=%u", p->sx, p->sy, p->sw, p->sh);
    out_str(g, "\x1b\\");
  }

  *out_len = g->len;
  return g->out; /* borrowed: valid until the next gfx_begin */
}

size_t gfx_placements(const graphics_t *g, const gfx_place_t **out) {
  *out = g->places;
  return g->nplaces;
}

size_t gfx_images(const graphics_t *g, const gfx_image_t **out) {
  *out = g->imgs;
  return g->nimgs;
}
