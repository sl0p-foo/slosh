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

/* Per pane. Small and fixed: a stack this deep is already a lot of passes. */
#define SHADE_MAX 4

typedef struct {
  uint16_t x, y;       /* cell position within the content rect, 0-based */
  uint16_t cols, rows; /* content size, so an effect can be positional */
  int64_t now_ms;      /* for anything animated */
  bool focused;

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
};

/* Build a shader by registry name. False if the name is not a built-in. */
bool shader_make(shader_t *out, const char *kind, color_t color,
                 uint8_t amount);
/* Iterate the registry: the i'th name, or NULL past the end. */
const char *shader_kind(size_t i);

/* Run `n` shaders in order over the w×h rect at (x0,y0). `base` supplies
 * everything in the context except the per-cell position and the size. */
void shade_apply(screen_t *s, const shader_t *shaders, size_t n, uint16_t x0,
                 uint16_t y0, uint16_t w, uint16_t h, const shade_ctx_t *base);

#endif /* SL0PPTY_SHADER_H */
