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

/* A placement the client was shown and must now be told to forget. Kept in
 * its own queue rather than deleted inline because the bytes of a frame can
 * fail to reach the client (the server's outbox has a ceiling): a deletion
 * that goes out with a dropped frame is a placement the client draws forever,
 * pinned to a spot on the screen no scrolling will move. The queue survives
 * until gfx_commit() says the client actually heard it. */
typedef struct {
  uint32_t out_id, place_id;
} gfx_dead_t;

struct graphics {
  gfx_image_t *imgs;
  size_t nimgs, imgcap;
  gfx_place_t *places;
  size_t nplaces, placecap;
  uint32_t next_out_id;

  gfx_dead_t *dead; /* deletions owed to the client */
  size_t ndead, deadcap;
  size_t *txed; /* imgs[] indices transmitted in the frame being built */
  size_t ntxed, txedcap;

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
  free(g->dead);
  free(g->txed);
  free(g->out);
  free(g);
}

void gfx_reset(graphics_t *g) {
  for (size_t i = 0; i < g->nimgs; i++) g->imgs[i].sent = false;
  g->nplaces = 0;
  /* A fresh client has no placements to delete and has seen no frame. */
  g->ndead = 0;
  g->ntxed = 0;
}

void gfx_forget_pane(graphics_t *g, uint32_t pane) {
  size_t keep = 0;
  for (size_t i = 0; i < g->nimgs; i++)
    if (g->imgs[i].pane != pane) g->imgs[keep++] = g->imgs[i];
  g->nimgs = keep;
}

void gfx_begin(graphics_t *g) {
  /* A flushed frame nobody committed was never delivered: the `graphics`
   * control command renders the stream for a CLI, not for the client, and
   * anything it claimed to transmit the client still has not seen. */
  if (g->ntxed) gfx_commit(g, false);
  for (size_t i = 0; i < g->nplaces; i++) g->places[i].live = false;
  g->len = 0;
  if (g->out) g->out[0] = 0;
  /* Saved here rather than in gfx_flush(): transmissions are emitted as
   * images are recorded, so by flush time they are already in the buffer and
   * a save written then would not cover them. See gfx_flush() for why the
   * whole stream has to be cursor-neutral.
   *
   * Split deliberately: "\x1b7" is a *single* hex escape, 0x1b7, which is out
   * of range for a char -- the compiler catches that one, but the same shape
   * with a digit that stays in range ("\x1b1") compiles and emits one wrong
   * byte. Escape then digit is always two strings. */
  out_str(g, "\x1b"
             "7"); /* DECSC */
}

static gfx_image_t *img_find(graphics_t *g, uint32_t pane, uint32_t src_id) {
  for (size_t i = 0; i < g->nimgs; i++)
    if (g->imgs[i].pane == pane && g->imgs[i].src_id == src_id)
      return &g->imgs[i];
  return NULL;
}

/* a=t: transmit without placing, so the same bytes can be placed repeatedly.
 *
 * What goes out is always *decoded* pixels, because that is the only form
 * lib-vt keeps: a PNG is decoded on the way in (D18) and we never see the
 * original bytes again. So a program that uploads an 819KB png costs 3.3MB of
 * base64 on the wire here -- once per image, and again after a reattach,
 * which calls gfx_reset() and clears `sent`. Fine for a splash screen, not
 * fine for anything that uploads a stream of photographs; if that ever shows
 * up, the fix is to keep the source bytes ourselves and pass f=100 straight
 * through rather than to make this loop cleverer. Nobody has felt it yet. */
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
  /* Provisionally: gfx_commit() unwinds this if the frame never arrives,
   * because `sent` on an image the client does not have means every future
   * placement of it silently draws nothing. Indices, not pointers -- the
   * array reallocates as the same frame records more images. */
  if (g->ntxed == g->txedcap) {
    g->txedcap = g->txedcap ? g->txedcap * 2 : 8;
    g->txed = realloc(g->txed, g->txedcap * sizeof *g->txed);
  }
  g->txed[g->ntxed++] = (size_t)(img - g->imgs);
}

void gfx_place(graphics_t *g, const gfx_req_t *req) {
  if (!g || !req) return;
  const uint32_t pane = req->pane, src_id = req->src_id;
  const uint64_t gen = req->gen;
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
    transmit(g, img, req->px_w, req->px_h, req->format, req->compression,
             req->data, req->data_len);

  uint32_t pid = req->place_id ? req->place_id : 1;
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
  slot->col = req->col;
  slot->row = req->row;
  slot->cols = req->cols;
  slot->rows = req->rows;
  slot->x_off = req->x_off;
  slot->y_off = req->y_off;
  slot->scale_cols = req->scale_cols;
  slot->scale_rows = req->scale_rows;
  slot->sx = req->sx;
  slot->sy = req->sy;
  slot->sw = req->sw;
  slot->sh = req->sh;
  slot->live = true;
}

char *gfx_flush(graphics_t *g, size_t *out_len) {
  /* This stream must leave the cursor exactly where it found it.
   *
   * Placing an image means parking the cursor on the target cell first, and
   * these bytes go out *after* the cell diff (so a repainted cell cannot land
   * on top of a placement) -- which is also after the frame has put the real
   * cursor where the focused pane wants it. Without the save/restore around
   * this, the last thing the terminal is told each frame is "go to wherever
   * the last image is", and the cursor sits there instead: in another pane,
   * moving about with the picture. It looks exactly like the shell you are
   * typing into has lost its cursor, which is how it was reported.
   *
   * DECSC/DECRC rather than tracking the position here, because the frame's
   * cursor is the screen's business and this file should not have to know it.
   * The save is written by gfx_begin(); an empty frame drops both below and
   * still costs nothing. */
  const size_t before = 2; /* the DECSC gfx_begin() wrote */

  /* Placements that were on screen last frame and are not now must be told to
   * go away; a terminal will happily keep drawing an image nobody owns. They
   * queue rather than emit-and-forget: the model prunes them here whether or
   * not the bytes ever reach the client, so the deletion must survive a
   * dropped frame and be said again until gfx_commit() confirms delivery. */
  size_t keep = 0;
  for (size_t i = 0; i < g->nplaces; i++) {
    gfx_place_t *p = &g->places[i];
    if (p->live) {
      g->places[keep++] = *p;
      continue;
    }
    bool queued = false;
    for (size_t j = 0; j < g->ndead; j++)
      if (g->dead[j].out_id == p->out_id && g->dead[j].place_id == p->place_id)
        queued = true;
    if (!queued) {
      if (g->ndead == g->deadcap) {
        g->deadcap = g->deadcap ? g->deadcap * 2 : 8;
        g->dead = realloc(g->dead, g->deadcap * sizeof *g->dead);
      }
      g->dead[g->ndead++] = (gfx_dead_t){p->out_id, p->place_id};
    }
  }
  g->nplaces = keep;

  /* Deletions before placements: a placement that died undelivered and then
   * came back is deleted and re-placed in the same stream, which nets out. */
  for (size_t i = 0; i < g->ndead; i++)
    out_fmt(g, "\x1b_Ga=d,d=i,q=2,i=%u,p=%u\x1b\\", g->dead[i].out_id,
            g->dead[i].place_id);

  /* Then (re)place everything that is. C=1 keeps the cursor where the text
   * renderer left it, which matters because we are interleaving with a diff. */
  for (size_t i = 0; i < g->nplaces; i++) {
    gfx_place_t *p = &g->places[i];
    out_fmt(g, "\x1b[%u;%uH", p->row + 1, p->col + 1);
    out_fmt(g, "\x1b_Ga=p,q=2,C=1,i=%u,p=%u", p->out_id, p->place_id);
    /* c=/r= mean *scale into this many cells*, so they are passed on only
     * when the program asked for them. Sending the cell count a natural-size
     * image happens to cover looks identical in a still picture and makes a
     * moving one change size, because that count goes up by one whenever the
     * image straddles one more cell boundary. */
    if (p->scale_cols || p->scale_rows)
      out_fmt(g, ",c=%u,r=%u", p->scale_cols, p->scale_rows);
    /* Where the image sits inside its first cell. Without these an image can
     * only ever land on a cell boundary, so anything moving smoothly arrives
     * in steps of a whole cell -- eight pixels across, sixteen down. */
    if (p->x_off) out_fmt(g, ",X=%u", p->x_off);
    if (p->y_off) out_fmt(g, ",Y=%u", p->y_off);
    /* The source rectangle is what actually crops; c/r alone would scale. */
    if (p->sw || p->sh)
      out_fmt(g, ",x=%u,y=%u,w=%u,h=%u", p->sx, p->sy, p->sw, p->sh);
    out_str(g, "\x1b\\");
  }

  if (g->len == before) {
    /* Nothing happened this frame: drop the save too, so an idle session with
     * no images on screen emits not one byte. */
    g->len = 0;
    if (g->out) g->out[0] = 0;
  } else {
    out_str(g, "\x1b"
               "8"); /* DECRC */
  }

  *out_len = g->len;
  return g->out; /* borrowed: valid until the next gfx_begin */
}

void gfx_commit(graphics_t *g, bool delivered) {
  if (delivered) {
    g->ndead = 0; /* the client heard the deletions; stop repeating them */
  } else {
    /* The transmissions in that frame never happened as far as the client
     * is concerned; forget we made them so the next frame sends them again.
     * Deletions stay queued for the same reason. Placements need nothing:
     * every live one is re-emitted every frame anyway. */
    for (size_t i = 0; i < g->ntxed; i++)
      if (g->txed[i] < g->nimgs) g->imgs[g->txed[i]].sent = false;
  }
  g->ntxed = 0;
}

size_t gfx_placements(const graphics_t *g, const gfx_place_t **out) {
  *out = g->places;
  return g->nplaces;
}

size_t gfx_images(const graphics_t *g, const gfx_image_t **out) {
  *out = g->imgs;
  return g->nimgs;
}
