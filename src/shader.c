/* The shader pass, the built-in shaders, and loading more of them from disk.
 * See shader.h for the model and shader_abi.h for what a plugin sees. */
#define _GNU_SOURCE
#include "shader.h"

#include <dlfcn.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expr.h"

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

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

/* shader_def_t rather than a struct of its own, so a built-in and a loaded
 * shader are the same kind of thing and lookup does not have two shapes. */
static const shader_def_t REGISTRY[] = {
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

#define NBUILTIN (sizeof REGISTRY / sizeof *REGISTRY)

/* ---- loaded shaders -----------------------------------------------------
 *
 * Kept beside the built-ins rather than merged into them: a built-in is a
 * fixed table the compiler can put in .rodata, and this is a list that grows
 * at runtime. Lookup asks the built-ins first, so a plugin cannot shadow
 * `dim` and change what every existing config means.
 *
 * Both the name and the function belong to the loaded library, which is why
 * nothing here is ever freed or unloaded: a shader_t copied into a config or
 * onto a pane holds both pointers. */
static struct {
  shader_def_t *defs;
  size_t n, cap;
  char **paths; /* what has been loaded, so a reload does not load it twice */
  size_t npaths, pathcap;
} LOADED;

static const shader_def_t *find_kind(const char *kind) {
  for (size_t i = 0; i < NBUILTIN; i++)
    if (strcmp(REGISTRY[i].name, kind) == 0) return &REGISTRY[i];
  for (size_t i = 0; i < LOADED.n; i++)
    if (strcmp(LOADED.defs[i].name, kind) == 0) return &LOADED.defs[i];
  return NULL;
}

bool shader_make(shader_t *out, const char *kind, color_t color,
                 uint8_t amount) {
  return shader_make_p(out, kind, color, amount, 0);
}

bool shader_make_p(shader_t *out, const char *kind, color_t color,
                   uint8_t amount, uint16_t param) {
  if (!out || !kind) return false;
  const shader_def_t *d = find_kind(kind);
  if (!d) return false;
  /* channels left 0, which the pass reads as both: a shader made here has no
   * opinion about that, and the config sets it when the config has one. */
  *out = (shader_t){.kind = d->name, .fn = d->fn, .color = color,
                    .amount = amount, .param = param};
  return true;
}

const char *shader_kind(size_t i) {
  if (i < NBUILTIN) return REGISTRY[i].name;
  i -= NBUILTIN;
  return i < LOADED.n ? LOADED.defs[i].name : NULL;
}

/* ---- loading ------------------------------------------------------------ */

static bool already_loaded(const char *path) {
  for (size_t i = 0; i < LOADED.npaths; i++)
    if (strcmp(LOADED.paths[i], path) == 0) return true;
  return false;
}

static void remember_path(const char *path) {
  if (LOADED.npaths == LOADED.pathcap) {
    LOADED.pathcap = LOADED.pathcap ? LOADED.pathcap * 2 : 8;
    LOADED.paths = realloc(LOADED.paths, LOADED.pathcap * sizeof *LOADED.paths);
  }
  LOADED.paths[LOADED.npaths++] = strdup(path);
}

/* One library. Everything that can be wrong with it is checked before a
 * single one of its shaders is registered, so a plugin is all-or-nothing:
 * half a bundle is harder to explain than none of it. */
static size_t load_one(const char *path, char *err, size_t errcap) {
  void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    if (err && !err[0])
      snprintf(err, errcap, "%s: %s", path, dlerror() ? dlerror() : "dlopen");
    return 0;
  }

  /* An object pointer is not a function pointer in ISO C, and dlsym returns
   * the former. POSIX requires the conversion to work; the cast through a
   * union is how it is written without the compiler being right to warn. */
  union {
    void *obj;
    const shader_plugin_t *(*fn)(void);
  } entry = {.obj = dlsym(lib, SL0PPTY_SHADER_PLUGIN_SYM)};

  if (!entry.obj) {
    if (err && !err[0])
      snprintf(err, errcap, "%s: no " SL0PPTY_SHADER_PLUGIN_SYM, path);
    dlclose(lib);
    return 0;
  }

  const shader_plugin_t *t = entry.fn();
  if (!t || t->abi != SL0PPTY_SHADER_ABI) {
    if (err && !err[0])
      snprintf(err, errcap, "%s: shader ABI %u, we speak %u", path,
               t ? t->abi : 0, SL0PPTY_SHADER_ABI);
    dlclose(lib);
    return 0;
  }
  /* The version says which contract; the sizes say whether the plugin really
   * has that contract's structs. They differ when someone edits their copy of
   * shader_abi.h, which a version number alone reports as fine and a running
   * session reports as garbled colours or a crash. */
  if (t->cell_size != sizeof(cell_t) || t->ctx_size != sizeof(shade_ctx_t) ||
      t->shader_size != sizeof(shader_t)) {
    if (err && !err[0])
      snprintf(err, errcap, "%s: built against different structs", path);
    dlclose(lib);
    return 0;
  }
  if (t->count && !t->shaders) {
    if (err && !err[0]) snprintf(err, errcap, "%s: empty shader table", path);
    dlclose(lib);
    return 0;
  }

  for (size_t i = 0; i < t->count; i++) {
    const shader_def_t *d = &t->shaders[i];
    if (!d->name || !*d->name || !d->fn) {
      if (err && !err[0]) snprintf(err, errcap, "%s: nameless shader", path);
      continue;
    }
    if (find_kind(d->name)) {
      /* Refused rather than replacing: a config naming `dim` means the `dim`
       * it was written against, and a plugin that could redefine it would
       * change effects nobody asked it to touch. */
      if (err && !err[0])
        snprintf(err, errcap, "%s: shader %s already exists", path, d->name);
      continue;
    }
    if (LOADED.n == LOADED.cap) {
      LOADED.cap = LOADED.cap ? LOADED.cap * 2 : 8;
      LOADED.defs = realloc(LOADED.defs, LOADED.cap * sizeof *LOADED.defs);
    }
    LOADED.defs[LOADED.n++] = *d;
  }

  /* The library stays mapped for the life of the session, on purpose: see
   * shader_load_dir()'s comment in the header. */
  return t->count;
}

size_t shader_load_dir(const char *dir, char *err, size_t errcap) {
  if (err && errcap) err[0] = 0;
  if (!dir || !*dir) return 0;

  char pattern[1024];
  snprintf(pattern, sizeof pattern, "%s/*.so", dir);

  glob_t g = {0};
  int rc = glob(pattern, 0, NULL, &g);
  if (rc != 0) {
    globfree(&g);
    return 0; /* no directory, or nothing in it: the normal case */
  }

  size_t added = 0;
  for (size_t i = 0; i < g.gl_pathc; i++) {
    if (already_loaded(g.gl_pathv[i])) continue;
    /* Remembered whether or not it loads. A library that fails today fails
     * every reload, and saying so once is a warning while saying so on every
     * save is a broken editor. */
    remember_path(g.gl_pathv[i]);
    added += load_one(g.gl_pathv[i], err, errcap);
  }
  globfree(&g);
  return added;
}

/* ---- the pass ----------------------------------------------------------- */

void shade_apply(screen_t *s, const shader_t *shaders, size_t n, rect_t r,
                 const rect_t *hole, const shade_ctx_t *base) {
  if (!s || !shaders || !n || !r.w || !r.h) return;

  /* Clip once, here, so the inner loop can walk a row pointer instead of
   * bounds-checking every cell through screen_at(). That call was a fifth of
   * the pass: it re-derived the row base and re-checked a coordinate the loop
   * already guarantees, once per cell per shader. The layout never asks for a
   * rect that needs clipping; this is so that a caller that does gets a
   * smaller rect rather than an out-of-bounds write. */
  uint16_t x0 = r.x, y0 = r.y, w = r.w, h = r.h;
  if (x0 >= s->cols || y0 >= s->rows) return;
  if ((size_t)x0 + w > s->cols) w = (uint16_t)(s->cols - x0);
  if ((size_t)y0 + h > s->rows) h = (uint16_t)(s->rows - y0);
  if (!w || !h) return;

  /* The hole in this rect's own coordinates, as a half-open span per axis.
   * Computed once: the inner loop then asks two comparisons per cell, and an
   * empty hole leaves an empty span that nothing is ever inside. */
  uint16_t hx0 = 0, hx1 = 0, hy0 = 0, hy1 = 0;
  if (hole && hole->w && hole->h && hole->x + hole->w > x0 &&
      hole->y + hole->h > y0) {
    hx0 = (uint16_t)(hole->x > x0 ? hole->x - x0 : 0);
    hy0 = (uint16_t)(hole->y > y0 ? hole->y - y0 : 0);
    long ex = (long)hole->x + hole->w - x0, ey = (long)hole->y + hole->h - y0;
    hx1 = (uint16_t)(ex > w ? w : ex);
    hy1 = (uint16_t)(ey > h ? h : ey);
  }

  shade_ctx_t ctx = base ? *base : (shade_ctx_t){0};
  ctx.cols = w;
  ctx.rows = h;

  /* `n` is the caller's to bound: SHADE_MAX is how many a pane may have
   * *attached*, and a caller may legitimately pass that many plus the one the
   * session derived for this frame. Capping here would silently drop it. */
  for (size_t i = 0; i < n; i++) {
    const shader_t *sh = &shaders[i];
    if (!sh->kind || !sh->fn) continue;

    /* An expression's amount is computed here, not inside the shader, so no
     * shader — built in or loaded — has to know that its strength might be a
     * function of position. `local` carries the per-cell value; it is copied
     * once per pass rather than per cell, because copying a shader_t for every
     * cell would cost more than evaluating the expression. */
    shader_t local = *sh;
    const uint8_t *map = NULL;
    expr_env_t env = {0};
    bool per_cell = false;
    /* Which colours this pass keeps. Per shader, not per cell: it is one test
     * of a byte, and the inner loop is walked tens of thousands of times a
     * frame. 0 means both, so a zeroed shader_t behaves as it always did. */
    bool keep_fg = !sh->channels || (sh->channels & SHADE_FG);
    bool keep_bg = !sh->channels || (sh->channels & SHADE_BG);

    if (sh->amount_expr) {
      env.cols = w;
      env.rows = h;
      env.curx = ctx.has_cursor ? ctx.cursor_x : 0;
      env.cury = ctx.has_cursor ? ctx.cursor_y : 0;
      env.cursor = ctx.has_cursor;
      env.focused = ctx.focused;
      env.t = ctx.now_ms;
      env.since = ctx.state_ms;
      if (expr_deps(sh->amount_expr) == 0) {
        local.amount = (uint8_t)clamp255(expr_constant(sh->amount_expr));
      } else {
        uint8_t *m = NULL;
        /* The map is the whole reason an interpreter is affordable: a program
         * that does not read the clock is evaluated once per cell and then
         * reused for as long as nothing it reads has changed. One that does
         * read the clock is evaluated per cell, per frame, which is what
         * asking for animation costs. */
        if (expr_amount_map(sh->amount_expr, &env, &m)) map = m;
        else per_cell = true;
      }
    }

    for (uint16_t y = 0; y < h; y++) {
      cell_t *row = &s->cur[(size_t)(y0 + y) * s->cols + x0];
      ctx.y = y;
      bool y_in_hole = y >= hy0 && y < hy1;
      for (uint16_t x = 0; x < w; x++) {
        /* Inside the hole: not this pass's cell. Skipped before anything is
         * materialised on it, so a frame pass leaves the contents exactly as
         * the content pass left them. */
        if (y_in_hole && x >= hx0 && x < hx1) {
          x = (uint16_t)(hx1 - 1); /* jump the span rather than walk it */
          continue;
        }
        cell_t *c = &row[x];
        /* width 0 is the tail half of a wide cell: never painted, so shading
         * it would be computing a colour nothing can display. */
        if (!c->width) continue;

        /* What the cell was, for whichever colour this pass does not keep.
         * Taken before materialising, so a colour the terminal was drawing in
         * its own default goes back to *unset* rather than to our idea of it —
         * the difference between recolouring a glyph and painting a rectangle
         * behind it. */
        color_t was_fg = c->fg, was_bg = c->bg;

        /* Materialised per shader rather than once for the chain, so a shader
         * added later cannot observe a half-resolved cell. Idempotent: once
         * set, this does nothing. */
        if (!c->fg.set) c->fg = ctx.default_fg;
        if (!c->bg.set) c->bg = ctx.default_bg;

        ctx.x = x;
        if (map) {
          local.amount = map[(size_t)y * w + x];
        } else if (per_cell) {
          env.x = x;
          env.y = y;
          local.amount = (uint8_t)clamp255(expr_eval(sh->amount_expr, &env));
        }
        local.fn(&local, &ctx, c);

        if (!keep_fg) c->fg = was_fg;
        if (!keep_bg) c->bg = was_bg;
      }
    }
  }
}
