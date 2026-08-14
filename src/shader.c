/* The shader pass and the built-in shaders. See shader.h for the model. */
#include "shader.h"

#include <string.h>

/* Integer maths throughout: this runs per cell per pass per frame, and a float
 * here would buy nothing a byte channel can tell the difference between. */
static uint8_t mix8(uint8_t from, uint8_t to, uint8_t amount) {
  return (uint8_t)(((int)from * (255 - amount) + (int)to * amount) / 255);
}

static color_t mix(color_t from, color_t to, uint8_t amount) {
  return (color_t){true, mix8(from.r, to.r, amount),
                   mix8(from.g, to.g, amount), mix8(from.b, to.b, amount)};
}

/* Rec. 601 luma. The green weight is not a rounding error: the eye is far more
 * sensitive to green, and averaging the channels instead produces a grey that
 * reads as the wrong brightness. */
static uint8_t luma(color_t c) {
  return (uint8_t)(((int)c.r * 77 + (int)c.g * 150 + (int)c.b * 29) >> 8);
}

/* ---- the built-ins ------------------------------------------------------ */

static void sh_dim(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  (void)ctx;
  const color_t black = {true, 0, 0, 0};
  c->fg = mix(c->fg, black, sh->amount);
  c->bg = mix(c->bg, black, sh->amount);
}

static void sh_grayscale(const shader_t *sh, const shade_ctx_t *ctx,
                         cell_t *c) {
  (void)ctx;
  uint8_t f = luma(c->fg), b = luma(c->bg);
  c->fg = mix(c->fg, (color_t){true, f, f, f}, sh->amount);
  c->bg = mix(c->bg, (color_t){true, b, b, b}, sh->amount);
}

static void sh_tint(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  (void)ctx;
  c->fg = mix(c->fg, sh->color, sh->amount);
  c->bg = mix(c->bg, sh->color, sh->amount);
}

static const struct {
  const char *name;
  shade_fn fn;
} REGISTRY[] = {
    {"dim", sh_dim},
    {"grayscale", sh_grayscale},
    {"tint", sh_tint},
};

bool shader_make(shader_t *out, const char *kind, color_t color,
                 uint8_t amount) {
  if (!out || !kind) return false;
  for (size_t i = 0; i < sizeof REGISTRY / sizeof *REGISTRY; i++) {
    if (strcmp(REGISTRY[i].name, kind) != 0) continue;
    *out = (shader_t){REGISTRY[i].name, REGISTRY[i].fn, color, amount};
    return true;
  }
  return false;
}

const char *shader_kind(size_t i) {
  return i < sizeof REGISTRY / sizeof *REGISTRY ? REGISTRY[i].name : NULL;
}

/* ---- the pass ----------------------------------------------------------- */

void shade_apply(screen_t *s, const shader_t *shaders, size_t n, uint16_t x0,
                 uint16_t y0, uint16_t w, uint16_t h, const shade_ctx_t *base) {
  if (!s || !shaders || !n || !w || !h) return;

  shade_ctx_t ctx = base ? *base : (shade_ctx_t){0};
  ctx.cols = w;
  ctx.rows = h;

  for (size_t i = 0; i < n && i < SHADE_MAX; i++) {
    const shader_t *sh = &shaders[i];
    if (!sh->kind || !sh->fn) continue;

    for (uint16_t y = 0; y < h; y++) {
      for (uint16_t x = 0; x < w; x++) {
        cell_t *c = screen_at(s, (uint16_t)(x0 + x), (uint16_t)(y0 + y));
        /* width 0 is the tail half of a wide cell: never painted, so shading
         * it would be computing a colour nothing can display. */
        if (!c || !c->width) continue;

        /* Materialised per shader rather than once for the chain, so a shader
         * added later cannot observe a half-resolved cell. Idempotent: once
         * set, this does nothing. */
        if (!c->fg.set) c->fg = ctx.default_fg;
        if (!c->bg.set) c->bg = ctx.default_bg;

        ctx.x = x;
        ctx.y = y;
        sh->fn(sh, &ctx, c);
      }
    }
  }
}
