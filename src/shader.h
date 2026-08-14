/* Terminal shaders: per-cell colour computation over a pane's CONTENTS.
 *
 * A shader is a pure function from (cell, position, parameters) to that cell's
 * colours. It runs after pane_compose() has written the pane's viewport into
 * the screen and before the split guide is drawn over it, which is what makes
 * "contents, not chrome" fall out of the existing paint order rather than
 * needing a rule: the frame is painted before and lives outside the content
 * rect, and an affordance drawn after stays legible on top of a dimmed pane.
 *
 * Cells only. A shader may write fg, bg and attrs; it may not touch text, len
 * or width. Rewriting text would desync selection and copy (which read the
 * terminal, not the screen) and could break the wide-cell invariant.
 *
 * Each cell is shaded independently — no neighbours — so effects are limited
 * to what position and colour can express. That is most of them: tint, dim,
 * grayscale, gradients, vignettes, anything positional. Convolutions (blur,
 * bloom) would need a second buffer and are deliberately not possible here.
 *
 * Shaders chain: each is a complete pass, so `grayscale` then `tint` differs
 * from `tint` then `grayscale`, and both are expressible.
 */
#ifndef SL0PPTY_SHADER_H
#define SL0PPTY_SHADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sl0ppty.h"

/* Per pane, and separately per config. Small and fixed: a stack this deep is
 * already a lot of passes over the same cells. */
#define SHADE_MAX 4
/* What one pane can end up running: the ones every pane gets from the config,
 * the ones attached to this pane, and the one or two the session derived for
 * this frame. */
#define SHADE_CHAIN_MAX (SHADE_MAX * 2 + 2)

typedef struct {
  uint16_t x, y;       /* cell position within the content rect, 0-based */
  uint16_t cols, rows; /* content size, so an effect can be positional */
  int64_t now_ms;      /* for anything animated */
  bool focused;

  /* Where the cursor is, in the same rect-relative space as x/y. Only ever
   * set for the pane that owns it, so an effect that follows the cursor does
   * not chase another pane's. */
  bool has_cursor;
  uint16_t cursor_x, cursor_y;

  /* What "terminal default" means while shading. A cell whose colour is unset
   * is drawn in whatever the client's terminal calls default, and we never
   * learn that RGB — but most terminal text is default-coloured, so a shader
   * that skipped those cells would visibly do nothing. The pass materialises
   * them to these before running the chain. Configured, because guessing is
   * the one thing that would be worse. */
  color_t default_fg, default_bg;
} shade_ctx_t;

typedef struct shader shader_t;
typedef void (*shade_fn)(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c);

struct shader {
  const char *kind; /* registry name, NULL for an empty slot */
  shade_fn fn;
  color_t color;  /* the target colour, for shaders that have one */
  uint8_t amount; /* strength, 0..255; 0 is identity, 255 is fully applied */
  /* One number whose meaning is the shader's own, because a second parameter
   * that is a column for one effect and a radius for another is not really
   * two things:
   *   ruler     the column to mark          margin  first column to dim
   *   zebra     rows per band               spotlight  radius in columns
   *   gradient  0 down, 1 up, 2 right, 3 left
   */
  uint16_t param;
};

/* Build a shader by registry name. False if the name is not a built-in. */
bool shader_make(shader_t *out, const char *kind, color_t color,
                 uint8_t amount);
/* The same, for the shaders that take a number of their own. */
bool shader_make_p(shader_t *out, const char *kind, color_t color,
                   uint8_t amount, uint16_t param);
/* Iterate the registry: the i'th name, or NULL past the end. */
const char *shader_kind(size_t i);

/* Run `n` shaders in order over the w×h rect at (x0,y0). `base` supplies
 * everything in the context except the per-cell position and the size. */
void shade_apply(screen_t *s, const shader_t *shaders, size_t n, uint16_t x0,
                 uint16_t y0, uint16_t w, uint16_t h, const shade_ctx_t *base);

#endif /* SL0PPTY_SHADER_H */
