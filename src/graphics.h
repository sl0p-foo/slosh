/* Kitty graphics passthrough.
 *
 * libghostty-vt parses the protocol and tracks every image and placement, so
 * a pane's images exist as data; what a multiplexer has to add is *re-emitting*
 * them to the client's terminal at the right place, with ids that cannot
 * collide between panes.
 *
 * This is why pi has to fall back to sixel under tmux and zellij: they drop
 * the images entirely. Here the native path works.
 */
#ifndef SL0PPTY_GRAPHICS_H
#define SL0PPTY_GRAPHICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct graphics graphics_t;

/* One image, as this session knows it. */
typedef struct {
  uint32_t pane;    /* which pane transmitted it */
  uint32_t src_id;  /* the id the program chose, unique only within its pane */
  uint32_t out_id;  /* the id we give the client, unique across the session */
  uint64_t gen;     /* image generation, so a changed image is re-sent */
  bool sent;
} gfx_image_t;

/* One placement on screen this frame. */
typedef struct {
  uint32_t out_id;
  uint32_t place_id;
  uint16_t col, row; /* screen cells */
  uint16_t cols, rows;
  /* Where the image starts *inside* its first cell, in pixels. This is what
   * makes motion smooth instead of snapping a cell at a time, and dropping it
   * is invisible in a still picture and obvious the moment anything moves. */
  uint32_t x_off, y_off;
  /* Cells to scale into, or 0 to draw at natural size. Only sent on when the
   * program asked for it. */
  uint16_t scale_cols, scale_rows;
  uint32_t sx, sy, sw, sh; /* source rectangle, in image pixels */
  bool live;
} gfx_place_t;

graphics_t *gfx_new(void);
void gfx_free(graphics_t *g);

/* Start a frame: everything placed last frame is marked stale. */
void gfx_begin(graphics_t *g);
/* Everything gfx_place() needs: where it goes, what it crops to, and the
 * image itself for the first time it is seen.
 *
 * A struct rather than the twenty positional arguments this used to be. Half
 * of them are uint32_t pixel quantities, so a transposed pair compiles
 * perfectly and shows up as a picture in slightly the wrong place — which is
 * exactly the bug class this file is for. */
typedef struct {
  uint32_t pane;    /* which pane it belongs to */
  uint32_t src_id;  /* the id the program chose */
  uint64_t gen;     /* image generation, so a changed image is re-sent */
  uint32_t place_id;
  uint16_t col, row;   /* screen cells */
  uint16_t cols, rows;             /* cells covered */
  uint16_t scale_cols, scale_rows; /* cells to scale into, 0 for natural */
  uint32_t x_off, y_off;   /* offset within the first cell, in pixels */
  uint32_t sx, sy, sw, sh; /* source rectangle, in image pixels */
  uint32_t px_w, px_h;     /* the image's own size */
  int format, compression;
  const uint8_t *data; /* NULL once the client has it */
  size_t data_len;
} gfx_req_t;

/* Record a visible placement, transmitting the image if the client has not
 * seen it yet. `data` may be NULL when the image is already transmitted. */
void gfx_place(graphics_t *g, const gfx_req_t *req);
/* End a frame: emit transmits, placements and deletions. Caller frees. */
char *gfx_flush(graphics_t *g, size_t *out_len);

/* Forget what the client has seen (it is a different client now). */
void gfx_reset(graphics_t *g);
/* Drop everything belonging to a pane that is gone. */
void gfx_forget_pane(graphics_t *g, uint32_t pane);

size_t gfx_placements(const graphics_t *g, const gfx_place_t **out);
size_t gfx_images(const graphics_t *g, const gfx_image_t **out);

#endif /* SL0PPTY_GRAPHICS_H */
