/* The shader pass and the built-ins. Pure cells in -> cells out, so pure
 * tests: no pty, no terminal, no timing. */
#include "shader.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

static void ok(const char *name, bool cond, const char *detail) {
  if (!cond) fails++;
  printf("%s %-52s %s\n", cond ? "ok  " : "FAIL", name, cond ? "" : detail);
}

static color_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (color_t){true, r, g, b};
}

static bool ceq(color_t c, uint8_t r, uint8_t g, uint8_t b) {
  return c.set && c.r == r && c.g == g && c.b == b;
}

static char detail[256];
static const char *shown(color_t c) {
  if (!c.set)
    snprintf(detail, sizeof detail, "(unset)");
  else
    snprintf(detail, sizeof detail, "#%02x%02x%02x", c.r, c.g, c.b);
  return detail;
}

/* A screen with one known cell at (x,y), everything else blank. */
static void put(screen_t *s, uint16_t x, uint16_t y, color_t fg, color_t bg) {
  cell_t *c = screen_at(s, x, y);
  c->text[0] = 'x';
  c->len = 1;
  c->width = 1;
  c->attrs = 0;
  c->fg = fg;
  c->bg = bg;
}

static shade_ctx_t base_ctx(void) {
  return (shade_ctx_t){.default_fg = rgb(0xff, 0xff, 0xff),
                       .default_bg = rgb(0x00, 0x00, 0x00)};
}

/* Applies one built-in over the whole screen. */
static void run1(screen_t *s, const char *kind, color_t color, uint8_t amount) {
  shader_t sh;
  if (!shader_make(&sh, kind, color, amount)) return;
  shade_ctx_t base = base_ctx();
  shade_apply(s, &sh, 1, (rect_t){0, 0, s->cols, s->rows}, NULL, &base);
}

static void run1p(screen_t *s, const char *kind, color_t color, uint8_t amount,
                  uint16_t param, const shade_ctx_t *ctx) {
  shader_t sh;
  if (!shader_make_p(&sh, kind, color, amount, param)) return;
  shade_ctx_t base = ctx ? *ctx : base_ctx();
  shade_apply(s, &sh, 1, (rect_t){0, 0, s->cols, s->rows}, NULL, &base);
}

/* Fills every cell mid-grey, so any change is a change the shader made. */
static void fill(screen_t *s, color_t fg, color_t bg) {
  for (uint16_t y = 0; y < s->rows; y++)
    for (uint16_t x = 0; x < s->cols; x++) put(s, x, y, fg, bg);
}

static uint8_t fg_at(screen_t *s, uint16_t x, uint16_t y) {
  return screen_at(s, x, y)->fg.r;
}

/* A probe shader, to see what the pass tells a shader about where it is. */
static struct {
  uint16_t x, y, cols, rows;
  int64_t now_ms;
  bool focused;
  color_t saw_fg;
  int calls;
} probe;

static void probe_fn(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  (void)sh;
  probe.calls++;
  probe.cols = ctx->cols;
  probe.rows = ctx->rows;
  probe.now_ms = ctx->now_ms;
  probe.focused = ctx->focused;
  if (ctx->x == 1 && ctx->y == 1) { /* remember one specific cell */
    probe.x = ctx->x;
    probe.y = ctx->y;
    probe.saw_fg = c->fg;
  }
  /* Write the position in, so the caller can prove each cell got its own. */
  c->bg = (color_t){true, (uint8_t)ctx->x, (uint8_t)ctx->y, 0};
}

int main(void) {
  screen_t s;

  /* ---- the registry ---- */
  {
    shader_t sh;
    ok("dim is a built-in", shader_make(&sh, "dim", (color_t){0}, 0), "");
    ok("grayscale is a built-in",
       shader_make(&sh, "grayscale", (color_t){0}, 0), "");
    ok("tint is a built-in", shader_make(&sh, "tint", (color_t){0}, 0), "");
    ok("an unknown kind is refused",
       !shader_make(&sh, "bloom", (color_t){0}, 0), "");
    ok("a NULL kind is refused", !shader_make(&sh, NULL, (color_t){0}, 0), "");

    ok("vignette is a built-in", shader_make(&sh, "vignette", (color_t){0}, 0),
       "");
    ok("gradient is a built-in", shader_make(&sh, "gradient", (color_t){0}, 0),
       "");
    ok("zebra is a built-in", shader_make(&sh, "zebra", (color_t){0}, 0), "");
    ok("ruler is a built-in", shader_make(&sh, "ruler", (color_t){0}, 0), "");
    ok("margin is a built-in", shader_make(&sh, "margin", (color_t){0}, 0), "");
    ok("spotlight is a built-in",
       shader_make(&sh, "spotlight", (color_t){0}, 0), "");

    size_t n = 0;
    while (shader_kind(n)) n++;
    ok("the registry enumerates every built-in", n == 9, "");
  }

  /* ---- amount is the whole strength scale ---- */
  {
    screen_init(&s, 4, 2);
    put(&s, 0, 0, rgb(0x80, 0x80, 0x80), rgb(0x40, 0x40, 0x40));
    run1(&s, "dim", (color_t){0}, 0);
    ok("amount 0 is the identity",
       ceq(screen_at(&s, 0, 0)->fg, 0x80, 0x80, 0x80),
       shown(screen_at(&s, 0, 0)->fg));
    screen_free(&s);

    screen_init(&s, 4, 2);
    put(&s, 0, 0, rgb(0x80, 0x80, 0x80), rgb(0x40, 0x40, 0x40));
    run1(&s, "dim", (color_t){0}, 255);
    ok("amount 255 dims fully to black",
       ceq(screen_at(&s, 0, 0)->fg, 0, 0, 0) &&
           ceq(screen_at(&s, 0, 0)->bg, 0, 0, 0),
       shown(screen_at(&s, 0, 0)->fg));
    screen_free(&s);

    screen_init(&s, 4, 2);
    put(&s, 0, 0, rgb(0xff, 0xff, 0xff), rgb(0, 0, 0));
    run1(&s, "dim", (color_t){0}, 128);
    cell_t *c = screen_at(&s, 0, 0);
    ok("halfway dims halfway", c->fg.r > 0x76 && c->fg.r < 0x84, shown(c->fg));
    screen_free(&s);
  }

  /* ---- grayscale ---- */
  {
    screen_init(&s, 4, 2);
    put(&s, 0, 0, rgb(0xff, 0x00, 0x00), rgb(0x00, 0x00, 0xff));
    run1(&s, "grayscale", (color_t){0}, 255);
    cell_t *c = screen_at(&s, 0, 0);
    ok("grey means the channels agree",
       c->fg.r == c->fg.g && c->fg.g == c->fg.b, shown(c->fg));
    ok("background is greyed too", c->bg.r == c->bg.g && c->bg.g == c->bg.b,
       shown(c->bg));
    screen_free(&s);

    /* Weighted, not averaged: pure green must come out brighter than pure
     * blue, or the grey reads at the wrong brightness. */
    screen_init(&s, 4, 2);
    put(&s, 0, 0, rgb(0x00, 0xff, 0x00), rgb(0x00, 0x00, 0xff));
    run1(&s, "grayscale", (color_t){0}, 255);
    c = screen_at(&s, 0, 0);
    ok("luma is weighted: green outranks blue", c->fg.r > c->bg.r,
       shown(c->fg));
    screen_free(&s);
  }

  /* ---- tint ---- */
  {
    screen_init(&s, 4, 2);
    put(&s, 0, 0, rgb(0x00, 0x00, 0x00), rgb(0x00, 0x00, 0x00));
    run1(&s, "tint", rgb(0xff, 0x00, 0x00), 255);
    ok("a full tint reaches the target colour",
       ceq(screen_at(&s, 0, 0)->fg, 0xff, 0, 0),
       shown(screen_at(&s, 0, 0)->fg));
    screen_free(&s);

    screen_init(&s, 4, 2);
    put(&s, 0, 0, rgb(0x00, 0x00, 0x00), rgb(0x00, 0x00, 0x00));
    run1(&s, "tint", rgb(0xff, 0x00, 0x00), 128);
    cell_t *c = screen_at(&s, 0, 0);
    ok("a half tint lands between", c->fg.r > 0x76 && c->fg.r < 0x84,
       shown(c->fg));
    screen_free(&s);
  }

  /* ---- the default-colour problem ---- */
  {
    screen_init(&s, 4, 2);
    put(&s, 0, 0, (color_t){0}, (color_t){0}); /* both "terminal default" */
    run1(&s, "grayscale", (color_t){0}, 255);
    cell_t *c = screen_at(&s, 0, 0);
    ok("an unset colour is materialised, not skipped", c->fg.set && c->bg.set,
       shown(c->fg));
    ok("default fg (white) greys to white", ceq(c->fg, 0xff, 0xff, 0xff),
       shown(c->fg));
    ok("default bg (black) greys to black", ceq(c->bg, 0, 0, 0), shown(c->bg));
    screen_free(&s);

    /* The point of materialising: dimming has to reach default-coloured text,
     * which is most text. */
    screen_init(&s, 4, 2);
    put(&s, 0, 0, (color_t){0}, (color_t){0});
    run1(&s, "dim", (color_t){0}, 255);
    ok("dim reaches default-coloured text",
       ceq(screen_at(&s, 0, 0)->fg, 0, 0, 0), shown(screen_at(&s, 0, 0)->fg));
    screen_free(&s);

    /* An unshaded pane must keep deferring to the terminal. */
    screen_init(&s, 4, 2);
    put(&s, 0, 0, (color_t){0}, (color_t){0});
    shade_ctx_t base = base_ctx();
    shade_apply(&s, NULL, 0, (rect_t){0, 0, 4, 2}, NULL, &base);
    ok("no shaders leaves defaults unset", !screen_at(&s, 0, 0)->fg.set,
       shown(screen_at(&s, 0, 0)->fg));
    screen_free(&s);
  }

  /* ---- what a shader must not touch ---- */
  {
    screen_init(&s, 4, 2);
    put(&s, 0, 0, rgb(0x80, 0x80, 0x80), rgb(0x40, 0x40, 0x40));
    cell_t *c = screen_at(&s, 0, 0);
    c->width = 2;
    c->len = 1;
    run1(&s, "dim", (color_t){0}, 255);
    ok("text, width and length survive a pass",
       c->width == 2 && c->len == 1 && c->text[0] == 'x', "");
    screen_free(&s);

    /* The tail half of a wide cell is never painted, so it is never shaded. */
    screen_init(&s, 4, 2);
    put(&s, 1, 0, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    screen_at(&s, 1, 0)->width = 0;
    run1(&s, "dim", (color_t){0}, 255);
    ok("a wide cell's tail is left alone",
       ceq(screen_at(&s, 1, 0)->fg, 0xff, 0xff, 0xff),
       shown(screen_at(&s, 1, 0)->fg));
    screen_free(&s);
  }

  /* ---- the pass stays inside its rect: this is what protects the chrome ---- */
  {
    screen_init(&s, 8, 4);
    for (uint16_t y = 0; y < 4; y++)
      for (uint16_t x = 0; x < 8; x++)
        put(&s, x, y, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));

    shader_t sh;
    shader_make(&sh, "dim", (color_t){0}, 255);
    shade_ctx_t base = base_ctx();
    shade_apply(&s, &sh, 1, (rect_t){2, 1, 4, 2}, NULL,
                &base); /* an inset rect */

    ok("a cell inside the rect is shaded",
       ceq(screen_at(&s, 2, 1)->fg, 0, 0, 0), shown(screen_at(&s, 2, 1)->fg));
    ok("its far corner too", ceq(screen_at(&s, 5, 2)->fg, 0, 0, 0),
       shown(screen_at(&s, 5, 2)->fg));
    ok("the cell left of the rect is untouched",
       ceq(screen_at(&s, 1, 1)->fg, 0xff, 0xff, 0xff),
       shown(screen_at(&s, 1, 1)->fg));
    ok("the cell right of it is untouched",
       ceq(screen_at(&s, 6, 1)->fg, 0xff, 0xff, 0xff),
       shown(screen_at(&s, 6, 1)->fg));
    ok("the row above is untouched",
       ceq(screen_at(&s, 2, 0)->fg, 0xff, 0xff, 0xff),
       shown(screen_at(&s, 2, 0)->fg));
    ok("the row below is untouched",
       ceq(screen_at(&s, 2, 3)->fg, 0xff, 0xff, 0xff),
       shown(screen_at(&s, 2, 3)->fg));
    screen_free(&s);
  }

  /* ---- chaining ---- */
  {
    /* grayscale then tint is not tint then grayscale: the first flattens the
     * hue the second would have had to work with. */
    shader_t gray, tint;
    shader_make(&gray, "grayscale", (color_t){0}, 255);
    shader_make(&tint, "tint", rgb(0xff, 0x00, 0x00), 128);
    shade_ctx_t base = base_ctx();

    screen_init(&s, 2, 1);
    put(&s, 0, 0, rgb(0x00, 0x80, 0x00), rgb(0, 0, 0));
    shader_t ab[2] = {gray, tint};
    shade_apply(&s, ab, 2, (rect_t){0, 0, 2, 1}, NULL, &base);
    color_t after_ab = screen_at(&s, 0, 0)->fg;
    screen_free(&s);

    screen_init(&s, 2, 1);
    put(&s, 0, 0, rgb(0x00, 0x80, 0x00), rgb(0, 0, 0));
    shader_t ba[2] = {tint, gray};
    shade_apply(&s, ba, 2, (rect_t){0, 0, 2, 1}, NULL, &base);
    color_t after_ba = screen_at(&s, 0, 0)->fg;
    screen_free(&s);

    ok("order matters: the chain is a sequence, not a set",
       !(after_ab.r == after_ba.r && after_ab.g == after_ba.g &&
         after_ab.b == after_ba.b),
       shown(after_ab));

    /* And a second pass sees the first one's output. */
    screen_init(&s, 2, 1);
    put(&s, 0, 0, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    shader_t twice[2];
    shader_make(&twice[0], "dim", (color_t){0}, 128);
    shader_make(&twice[1], "dim", (color_t){0}, 128);
    shade_apply(&s, twice, 2, (rect_t){0, 0, 2, 1}, NULL, &base);
    ok("two half-dims are darker than one", screen_at(&s, 0, 0)->fg.r < 0x70,
       shown(screen_at(&s, 0, 0)->fg));
    screen_free(&s);
  }

  /* ---- what the pass tells a shader ---- */
  {
    memset(&probe, 0, sizeof probe);
    screen_init(&s, 8, 4);
    for (uint16_t y = 0; y < 4; y++)
      for (uint16_t x = 0; x < 8; x++)
        put(&s, x, y, rgb(0x11, 0x22, 0x33), rgb(0, 0, 0));

    /* Strength 255: the probe ignores it, but a pass at 0 is an identity and
     * identities are skipped, so a traversal test has to ask for a real one. */
    shader_t sh = {.kind = "probe", .fn = probe_fn, .amount = 255};
    shade_ctx_t base = base_ctx();
    base.now_ms = 1234;
    base.focused = true;
    shade_apply(&s, &sh, 1, (rect_t){2, 1, 4, 2}, NULL, &base);

    ok("every cell in the rect is visited exactly once", probe.calls == 4 * 2,
       "");
    ok("the shader is told the content size, not the screen size",
       probe.cols == 4 && probe.rows == 2, "");
    ok("positions are rect-relative, so effects can be positional",
       probe.x == 1 && probe.y == 1, "");
    ok("and the cell at that position is the right one",
       ceq(probe.saw_fg, 0x11, 0x22, 0x33), shown(probe.saw_fg));

    ok("time is passed through for animation", probe.now_ms == 1234, "");
    ok("so is focus", probe.focused, "");

    /* Each cell got its own coordinates, not a single shared one. */
    ok("(0,0) of the rect is the screen cell the rect starts at",
       ceq(screen_at(&s, 2, 1)->bg, 0, 0, 0), shown(screen_at(&s, 2, 1)->bg));
    ok("and (3,1) is its opposite corner",
       ceq(screen_at(&s, 5, 2)->bg, 3, 1, 0), shown(screen_at(&s, 5, 2)->bg));
    screen_free(&s);
  }

  /* ---- zero strength is not a pass at all ---- */
  {
    memset(&probe, 0, sizeof probe);
    screen_init(&s, 4, 2); /* every colour unset: the terminal's own */
    shader_t idle = {.kind = "probe", .fn = probe_fn}; /* amount 0 */
    shade_ctx_t base = base_ctx();
    shade_apply(&s, &idle, 1, (rect_t){0, 0, 4, 2}, NULL, &base);

    ok("a pass at zero strength visits nothing", probe.calls == 0, "");
    /* `0 is identity` in the ABI, and the pass used to materialise a cell's
     * default colours before honouring that -- so a chain like `focused * 200`
     * repainted the frame of the pane it was leaving alone. */
    ok("and a default-coloured cell is left saying so",
       screen_at(&s, 0, 0)->fg.set == false &&
           screen_at(&s, 0, 0)->bg.set == false,
       "an identity pass materialised the terminal's default into ours");
    screen_free(&s);
  }

  /* ---- the hole: a frame is a rect minus its contents ---- */
  {
    memset(&probe, 0, sizeof probe);
    screen_init(&s, 6, 5);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));

    /* Strength 255: the probe ignores it, but a pass at 0 is an identity and
     * identities are skipped, so a traversal test has to ask for a real one. */
    shader_t sh = {.kind = "probe", .fn = probe_fn, .amount = 255};
    shade_ctx_t base = base_ctx();
    rect_t hole = {1, 1, 4, 3};
    shade_apply(&s, &sh, 1, (rect_t){0, 0, 6, 5}, &hole, &base);

    ok("a hole leaves the ring, and only the ring",
       probe.calls == 6 * 5 - 4 * 3, "");
    ok("a cell inside the hole is not visited at all",
       ceq(screen_at(&s, 2, 2)->bg, 0xff, 0xff, 0xff),
       shown(screen_at(&s, 2, 2)->bg));
    ok("the ring is still measured against the whole rect",
       probe.cols == 6 && probe.rows == 5, "");
    ok("so the far corner keeps the rect's own coordinates",
       ceq(screen_at(&s, 5, 4)->bg, 5, 4, 0), shown(screen_at(&s, 5, 4)->bg));
    ok("and a side cell beside the hole is the ring's",
       ceq(screen_at(&s, 0, 2)->bg, 0, 2, 0), shown(screen_at(&s, 0, 2)->bg));
    screen_free(&s);
  }

  /* A hole that is empty, or that misses, is no hole: a pane whose rect is too
   * small for a frame has no content rect to speak of, and asking for one
   * anyway must not silently skip half the pass. */
  {
    memset(&probe, 0, sizeof probe);
    screen_init(&s, 4, 2);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    /* Strength 255: the probe ignores it, but a pass at 0 is an identity and
     * identities are skipped, so a traversal test has to ask for a real one. */
    shader_t sh = {.kind = "probe", .fn = probe_fn, .amount = 255};
    shade_ctx_t base = base_ctx();
    rect_t empty = {1, 1, 0, 0};
    shade_apply(&s, &sh, 1, (rect_t){0, 0, 4, 2}, &empty, &base);
    ok("a zero-sized hole removes nothing", probe.calls == 4 * 2, "");

    memset(&probe, 0, sizeof probe);
    rect_t elsewhere = {40, 40, 4, 4};
    shade_apply(&s, &sh, 1, (rect_t){0, 0, 4, 2}, &elsewhere, &base);
    ok("nor does one that does not overlap", probe.calls == 4 * 2, "");

    /* Clipped rather than wrapped: a hole hanging off the rect removes the
     * part that is inside it and nothing else. */
    memset(&probe, 0, sizeof probe);
    rect_t over = {2, 0, 10, 10};
    shade_apply(&s, &sh, 1, (rect_t){0, 0, 4, 2}, &over, &base);
    ok("a hole larger than the rect is clipped to it", probe.calls == 2 * 2,
       "");
    screen_free(&s);
  }

  /* ---- channels: which of a cell's two colours a pass keeps ---- */
  {
    shader_t sh;
    shade_ctx_t base = base_ctx();
    const color_t red = {true, 0xff, 0, 0};

    screen_init(&s, 2, 1);
    fill(&s, rgb(0x40, 0x40, 0x40), rgb(0x20, 0x20, 0x20));
    shader_make(&sh, "tint", red, 255);
    sh.channels = SHADE_FG;
    shade_apply(&s, &sh, 1, (rect_t){0, 0, 2, 1}, NULL, &base);
    ok("channel fg tints the foreground",
       ceq(screen_at(&s, 0, 0)->fg, 0xff, 0, 0),
       shown(screen_at(&s, 0, 0)->fg));
    ok("...and puts the background back",
       ceq(screen_at(&s, 0, 0)->bg, 0x20, 0x20, 0x20),
       shown(screen_at(&s, 0, 0)->bg));

    fill(&s, rgb(0x40, 0x40, 0x40), rgb(0x20, 0x20, 0x20));
    sh.channels = SHADE_BG;
    shade_apply(&s, &sh, 1, (rect_t){0, 0, 2, 1}, NULL, &base);
    ok("channel bg tints the background",
       ceq(screen_at(&s, 0, 0)->bg, 0xff, 0, 0),
       shown(screen_at(&s, 0, 0)->bg));
    ok("...and leaves the foreground",
       ceq(screen_at(&s, 0, 0)->fg, 0x40, 0x40, 0x40),
       shown(screen_at(&s, 0, 0)->fg));

    /* The reason the mask exists. A cell the terminal draws in its own default
     * has no background of ours; mixing that towards a colour turns a
     * recoloured glyph into a painted rectangle, and it must come back *unset*
     * rather than as our idea of what default means. */
    screen_free(&s);
    screen_init(&s, 2, 1);
    put(&s, 0, 0, rgb(0x40, 0x40, 0x40), (color_t){0});
    put(&s, 1, 0, rgb(0x40, 0x40, 0x40), (color_t){0});
    sh.channels = SHADE_FG;
    shade_apply(&s, &sh, 1, (rect_t){0, 0, 2, 1}, NULL, &base);
    ok("an unset background stays unset", !screen_at(&s, 0, 0)->bg.set,
       shown(screen_at(&s, 0, 0)->bg));

    /* Zero is both, so every shader made before the mask existed behaves as it
     * did: the field is the pass's, and a shader that says nothing gets the
     * old answer. */
    fill(&s, rgb(0x40, 0x40, 0x40), rgb(0x20, 0x20, 0x20));
    sh.channels = 0;
    shade_apply(&s, &sh, 1, (rect_t){0, 0, 2, 1}, NULL, &base);
    ok("no mask means both",
       ceq(screen_at(&s, 0, 0)->fg, 0xff, 0, 0) &&
           ceq(screen_at(&s, 0, 0)->bg, 0xff, 0, 0),
       shown(screen_at(&s, 0, 0)->bg));
    screen_free(&s);
  }

  /* ---- positional: vignette ---- */
  {
    screen_init(&s, 41, 21);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    run1(&s, "vignette", (color_t){0}, 255);
    ok("the centre is untouched", fg_at(&s, 20, 10) == 0xff,
       shown(screen_at(&s, 20, 10)->fg));
    ok("a corner is the darkest point",
       fg_at(&s, 0, 0) < fg_at(&s, 10, 5) &&
           fg_at(&s, 10, 5) < fg_at(&s, 20, 10),
       shown(screen_at(&s, 0, 0)->fg));
    ok("it is symmetric left to right", fg_at(&s, 0, 10) == fg_at(&s, 40, 10),
       "");
    ok("and top to bottom", fg_at(&s, 20, 0) == fg_at(&s, 20, 20), "");
    /* A cell is about twice as tall as it is wide, so this pane (41x21) is
     * square on screen even though it is not square in cells. Its top edge and
     * its left edge are therefore the same distance from the middle, and come
     * out equally dark. Without the row doubling the left edge would be four
     * times further by the maths and much darker — so this equality is the
     * whole proof that the falloff is round in cells rather than in columns. */
    ok("a row counts double, so the falloff is round on screen",
       fg_at(&s, 20, 0) == fg_at(&s, 0, 10), shown(screen_at(&s, 20, 0)->fg));
    screen_free(&s);
  }

  /* ---- positional: gradient ---- */
  {
    screen_init(&s, 8, 8);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    run1p(&s, "gradient", (color_t){0}, 255, 0, NULL); /* down */
    ok("a downward gradient leaves the top row alone", fg_at(&s, 0, 0) == 0xff,
       shown(screen_at(&s, 0, 0)->fg));
    ok("and reaches the background by the bottom", fg_at(&s, 0, 7) == 0x00,
       shown(screen_at(&s, 0, 7)->fg));
    ok("it is uniform across a row", fg_at(&s, 0, 3) == fg_at(&s, 7, 3), "");
    screen_free(&s);

    screen_init(&s, 8, 8);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    run1p(&s, "gradient", (color_t){0}, 255, 2, NULL); /* rightward */
    ok("direction 2 runs left to right instead",
       fg_at(&s, 0, 0) == 0xff && fg_at(&s, 7, 0) == 0x00, "");
    screen_free(&s);
  }

  /* ---- positional: zebra ---- */
  {
    screen_init(&s, 4, 6);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    run1p(&s, "zebra", (color_t){0}, 255, 1, NULL);
    ok("even rows are left alone",
       fg_at(&s, 0, 0) == 0xff && fg_at(&s, 0, 2) == 0xff, "");
    ok("odd rows are darkened",
       fg_at(&s, 0, 1) == 0x00 && fg_at(&s, 0, 3) == 0x00, "");
    screen_free(&s);

    screen_init(&s, 4, 8);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    run1p(&s, "zebra", (color_t){0}, 255, 2, NULL);
    ok("a band of 2 alternates every two rows",
       fg_at(&s, 0, 0) == 0xff && fg_at(&s, 0, 1) == 0xff &&
           fg_at(&s, 0, 2) == 0x00 && fg_at(&s, 0, 3) == 0x00,
       "");
    screen_free(&s);
  }

  /* ---- positional: ruler ---- */
  {
    screen_init(&s, 10, 3);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0x00, 0x00, 0x00));
    run1p(&s, "ruler", rgb(0xff, 0x00, 0x00), 255, 4, NULL);
    ok("the marked column takes the colour",
       ceq(screen_at(&s, 4, 0)->bg, 0xff, 0, 0),
       shown(screen_at(&s, 4, 0)->bg));
    ok("down its whole height", ceq(screen_at(&s, 4, 2)->bg, 0xff, 0, 0), "");
    ok("its neighbours are untouched",
       ceq(screen_at(&s, 3, 0)->bg, 0, 0, 0) &&
           ceq(screen_at(&s, 5, 0)->bg, 0, 0, 0),
       "");
    /* A guide that recoloured the text would make the code it marks harder to
     * read, which is the opposite of the job. */
    ok("the text on it keeps its own colour",
       ceq(screen_at(&s, 4, 0)->fg, 0xff, 0xff, 0xff),
       shown(screen_at(&s, 4, 0)->fg));
    screen_free(&s);
  }

  /* ---- positional: margin ---- */
  {
    screen_init(&s, 10, 2);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    run1p(&s, "margin", (color_t){0}, 255, 6, NULL);
    ok("columns before the margin are untouched", fg_at(&s, 5, 0) == 0xff, "");
    ok("the margin column itself recedes", fg_at(&s, 6, 0) == 0x00, "");
    ok("and everything past it", fg_at(&s, 9, 0) == 0x00, "");
    screen_free(&s);
  }

  /* ---- positional: spotlight ---- */
  {
    shade_ctx_t ctx = base_ctx();
    ctx.has_cursor = true;
    ctx.cursor_x = 20;
    ctx.cursor_y = 10;

    screen_init(&s, 41, 21);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    run1p(&s, "spotlight", (color_t){0}, 255, 5, &ctx);
    ok("the cell under the cursor is untouched", fg_at(&s, 20, 10) == 0xff, "");
    ok("and so is everything inside the radius", fg_at(&s, 24, 10) == 0xff, "");
    ok("just past the radius the light has begun to fall away",
       fg_at(&s, 26, 10) < 0xff, shown(screen_at(&s, 26, 10)->fg));
    ok("and keeps falling with distance", fg_at(&s, 26, 10) > fg_at(&s, 28, 10),
       "");
    ok("the falloff is a ramp, not an edge",
       fg_at(&s, 25, 10) > fg_at(&s, 27, 10) &&
           fg_at(&s, 27, 10) > fg_at(&s, 29, 10),
       "");
    ok("far enough out it is fully dark", fg_at(&s, 35, 10) == 0x00, "");
    screen_free(&s);

    /* With no cursor there is nothing to centre on, so it does nothing at all
     * rather than guessing at the middle. */
    screen_init(&s, 41, 21);
    fill(&s, rgb(0xff, 0xff, 0xff), rgb(0xff, 0xff, 0xff));
    shade_ctx_t nocur = base_ctx();
    run1p(&s, "spotlight", (color_t){0}, 255, 5, &nocur);
    ok("without a cursor the spotlight does nothing",
       fg_at(&s, 0, 0) == 0xff && fg_at(&s, 40, 20) == 0xff, "");
    screen_free(&s);
  }

  /* ---- degenerate input ---- */
  {
    screen_init(&s, 4, 2);
    put(&s, 0, 0, rgb(0x80, 0x80, 0x80), rgb(0, 0, 0));
    shader_t sh;
    shader_make(&sh, "dim", (color_t){0}, 255);
    shade_ctx_t base = base_ctx();
    shade_apply(&s, &sh, 1, (rect_t){0, 0, 0, 0}, NULL, &base); /* empty rect */
    shade_apply(&s, &sh, 0, (rect_t){0, 0, 4, 2}, NULL, &base); /* no shaders */
    shade_apply(NULL, &sh, 1, (rect_t){0, 0, 4, 2}, NULL,
                &base); /* no screen */
    shade_apply(&s, &sh, 1, (rect_t){100, 100, 4, 2}, NULL,
                &base); /* rect off-screen */
    ok("a degenerate pass changes nothing and does not crash",
       ceq(screen_at(&s, 0, 0)->fg, 0x80, 0x80, 0x80),
       shown(screen_at(&s, 0, 0)->fg));
    screen_free(&s);
  }

  printf("\n%s (%d failures)\n", fails ? "FAILED" : "all green", fails);
  return fails ? 1 : 0;
}
