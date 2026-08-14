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

/* ---- positional --------------------------------------------------------- */

/* A terminal cell is about twice as tall as it is wide, so any effect that
 * claims to be round has to say so in cells: a row counts double, exactly as
 * the layout's own centre-distance does. */
static int dist2(int dx, int dy) { return dx * dx + (dy * 2) * (dy * 2); }

static void dim_by(cell_t *c, uint8_t a) {
  const color_t black = {true, 0, 0, 0};
  c->fg = mix(c->fg, black, a);
  c->bg = mix(c->bg, black, a);
}

/* Darken towards the edges. Falloff is on squared distance, which is both
 * cheaper than a square root and the curve a vignette wants anyway: flat
 * across the middle, then gathering pace. */
static void sh_vignette(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  int cx = ctx->cols / 2, cy = ctx->rows / 2;
  int far = dist2(cx, cy);
  if (far <= 0) return;
  int d = dist2((int)ctx->x - cx, (int)ctx->y - cy);
  if (d > far) d = far;
  dim_by(c, (uint8_t)((int)sh->amount * d / far));
}

/* Fade across the pane towards the background, so a pane reads as a surface
 * with a light on it rather than a rectangle of text. */
static void sh_gradient(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  int span, pos;
  switch (sh->param) {
    case 1: span = ctx->rows; pos = ctx->rows - 1 - ctx->y; break;
    case 2: span = ctx->cols; pos = ctx->x; break;
    case 3: span = ctx->cols; pos = ctx->cols - 1 - ctx->x; break;
    default: span = ctx->rows; pos = ctx->y; break;
  }
  if (span <= 1) return;
  uint8_t a = (uint8_t)((int)sh->amount * pos / (span - 1));
  c->fg = mix(c->fg, ctx->default_bg, a);
  c->bg = mix(c->bg, ctx->default_bg, a);
}

/* Horizontal banding. `param` rows per band, every other band darkened —
 * which at one row per band is also what a CRT scanline is, so there is one
 * shader here and not two. */
static void sh_zebra(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  uint16_t band = sh->param ? sh->param : 1;
  if (((ctx->y / band) & 1) == 0) return;
  dim_by(c, sh->amount);
}

/* A column guide. Background only: a ruler that recoloured the text would be
 * making the code it marks harder to read, which is the opposite of the job. */
static void sh_ruler(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  if (ctx->x != sh->param) return;
  c->bg = mix(c->bg, sh->color, sh->amount);
}

/* Everything past a column recedes, so an over-long line visibly runs out of
 * the part of the pane you meant to use. */
static void sh_margin(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  if (ctx->x < sh->param) return;
  dim_by(c, sh->amount);
}

/* Brightness falls away from the cursor. Inside the radius nothing happens at
 * all, so the text you are working on is never touched; beyond it the dimming
 * ramps up over the same distance again rather than switching on at an edge. */
static void sh_spotlight(const shader_t *sh, const shade_ctx_t *ctx,
                         cell_t *c) {
  if (!ctx->has_cursor) return;
  int r = sh->param ? sh->param : 10;
  int inner = r * r;
  int d = dist2((int)ctx->x - (int)ctx->cursor_x,
                (int)ctx->y - (int)ctx->cursor_y);
  if (d <= inner) return;
  int ramp = inner * 3;
  int over = d - inner;
  int k = over >= ramp ? 255 : over * 255 / ramp;
  dim_by(c, (uint8_t)((int)sh->amount * k / 255));
}

static const struct {
  const char *name;
  shade_fn fn;
} REGISTRY[] = {
    {"dim", sh_dim},
    {"grayscale", sh_grayscale},
    {"tint", sh_tint},
    {"vignette", sh_vignette},
    {"gradient", sh_gradient},
    {"zebra", sh_zebra},
    {"ruler", sh_ruler},
    {"margin", sh_margin},
    {"spotlight", sh_spotlight},
};

bool shader_make(shader_t *out, const char *kind, color_t color,
                 uint8_t amount) {
  return shader_make_p(out, kind, color, amount, 0);
}

bool shader_make_p(shader_t *out, const char *kind, color_t color,
                   uint8_t amount, uint16_t param) {
  if (!out || !kind) return false;
  for (size_t i = 0; i < sizeof REGISTRY / sizeof *REGISTRY; i++) {
    if (strcmp(REGISTRY[i].name, kind) != 0) continue;
    *out = (shader_t){REGISTRY[i].name, REGISTRY[i].fn, color, amount, param};
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

  /* `n` is the caller's to bound: SHADE_MAX is how many a pane may have
   * *attached*, and a caller may legitimately pass that many plus the one the
   * session derived for this frame. Capping here would silently drop it. */
  for (size_t i = 0; i < n; i++) {
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
