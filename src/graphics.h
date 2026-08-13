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
#ifndef SL0PTTY_GRAPHICS_H
#define SL0PTTY_GRAPHICS_H

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
  uint32_t sx, sy, sw, sh; /* source rectangle, in image pixels */
  bool live;
} gfx_place_t;

graphics_t *gfx_new(void);
void gfx_free(graphics_t *g);

/* Start a frame: everything placed last frame is marked stale. */
void gfx_begin(graphics_t *g);
/* Record a visible placement, transmitting the image if the client has not
 * seen it yet. `data` may be NULL when the image is already transmitted. */
void gfx_place(graphics_t *g, uint32_t pane, uint32_t src_id, uint64_t gen,
               uint32_t place_id, uint16_t col, uint16_t row, uint16_t cols,
               uint16_t rows, uint32_t sx, uint32_t sy, uint32_t sw,
               uint32_t sh, uint32_t px_w, uint32_t px_h, int format,
               int compression, const uint8_t *data, size_t data_len);
/* End a frame: emit transmits, placements and deletions. Caller frees. */
char *gfx_flush(graphics_t *g, size_t *out_len);

/* Forget what the client has seen (it is a different client now). */
void gfx_reset(graphics_t *g);
/* Drop everything belonging to a pane that is gone. */
void gfx_forget_pane(graphics_t *g, uint32_t pane);

size_t gfx_placements(const graphics_t *g, const gfx_place_t **out);
size_t gfx_images(const graphics_t *g, const gfx_image_t **out);

#endif /* SL0PTTY_GRAPHICS_H */
