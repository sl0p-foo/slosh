/* An example slosh shader plugin.
 *
 * Two effects, chosen to show the whole of what a shader is given: where the
 * cell is, how big the pane is, where the cursor is, what time it is, and the
 * three parameters the config can set (`color`, `amount`, and one number).
 *
 * Build it with `make`, drop the .so in ~/.config/slosh/shaders/, and name
 * it in your config exactly like a built-in:
 *
 *   shaders {
 *       checker amount=40 band=4
 *       pulse   amount=60 color="#ff5fd7"
 *   }
 *
 * The rules a shader must keep (they are the reason shaders can be this
 * simple):
 *
 *   - write c->fg, c->bg and c->attrs; never c->text, c->len or c->width
 *   - look at one cell only: no neighbours, no history, no other panes
 *   - return quickly and always. This runs per cell, per pass, per frame:
 *     tens of thousands of calls at 120Hz. It must not allocate, block, do
 *     I/O, or loop for a length it does not control.
 *
 * A shader is native code in the multiplexer's own process. It can do
 * anything the session can. Only install ones you trust.
 */
#include "shader_abi.h"

/* The mix every colour effect needs: `amount` of `to`, 0..255. */
static uint8_t mix8(uint8_t from, uint8_t to, uint8_t amount) {
  return (uint8_t)(((int)from * (255 - amount) + (int)to * amount) / 255);
}
static color_t mix(color_t from, color_t to, uint8_t amount) {
  return (color_t){true, mix8(from.r, to.r, amount), mix8(from.g, to.g, amount),
                   mix8(from.b, to.b, amount)};
}

/* A checkerboard, `param` cells to a square (`band=` in the config, which is
 * one of the names that number answers to). Positional only: it looks at
 * ctx->x and ctx->y and nothing else, which is the cheapest kind of effect
 * and the kind most worth writing. */
static void sh_checker(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  uint16_t n = sh->param ? sh->param : 4;
  /* Cells are about twice as tall as they are wide, so a square in cells is
   * half as many rows as columns. Ignore this and your checkerboard is a
   * pattern of letterboxes. */
  bool dark = (((ctx->x / n) + (ctx->y / (n / 2 ? n / 2 : 1))) & 1) != 0;
  if (!dark) return;
  const color_t black = {true, 0, 0, 0};
  c->fg = mix(c->fg, black, sh->amount);
  c->bg = mix(c->bg, black, sh->amount);
}

/* A slow pulse towards `color`, brightest at the cursor and fading with
 * distance from it. Uses ctx->now_ms, so it repaints continuously while it is
 * on screen — animation is allowed, but it is the expensive kind of effect
 * and it is worth knowing that is what you asked for. */
static void sh_pulse(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  if (!ctx->has_cursor) return;

  /* A triangle wave over four seconds: 0 -> 255 -> 0, no floating point and
   * no trig, because neither buys anything a byte channel can show. */
  int phase = (int)(ctx->now_ms % 4000);
  int wave = phase < 2000 ? phase * 255 / 2000 : (4000 - phase) * 255 / 2000;

  int dx = (int)ctx->x - (int)ctx->cursor_x;
  int dy = ((int)ctx->y - (int)ctx->cursor_y) * 2; /* cells, again */
  int d2 = dx * dx + dy * dy;
  int reach = 20 * 20;
  if (d2 >= reach) return;

  int near = (reach - d2) * 255 / reach;
  uint8_t a = (uint8_t)((int)sh->amount * wave / 255 * near / 255);
  c->fg = mix(c->fg, sh->color, a);
}

static const shader_def_t SHADERS[] = {
    {"checker", sh_checker},
    {"pulse", sh_pulse},
};

SLOSH_SHADER_PLUGIN("example", SHADERS)
