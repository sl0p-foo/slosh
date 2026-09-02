/* The shader model: everything a shader sees, and nothing else.
 *
 * The contract: a shader gets a cell and where it sits, and it cannot reach
 * anything else, because nothing else is declared here — no libghostty-vt,
 * no pty, no layout. (This file was once the ABI for loadable .so shader
 * plugins; those are gone — the static musl release builds cannot dlopen, so
 * a feature most binaries could not use was a feature in name only — but the
 * discipline of a small, closed surface is worth keeping.)
 */
#ifndef SLOSH_SHADER_ABI_H
#define SLOSH_SHADER_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Which of a cell's two colours a pass is allowed to keep. Enforced by the
 * pass, not by the shader: every shader writes whatever it writes, and the
 * cell's other colour is put back afterwards — so this works for a built-in
 * and for a loaded one, and no shader has to grow a variant of itself.
 *
 * Put back to what it *was*, which for a cell the terminal is drawing in its
 * own default colour means back to unset. That is the difference between
 * recolouring a border glyph and painting a dark rectangle behind it. */
enum {
  SHADE_FG = 1 << 0,
  SHADE_BG = 1 << 1,
  SHADE_BOTH = SHADE_FG | SHADE_BG, /* and 0 means this, so zeroed is normal */
};

enum {
  ATTR_BOLD = 1 << 0,
  ATTR_DIM = 1 << 1,
  ATTR_ITALIC = 1 << 2,
  ATTR_UNDERLINE = 1 << 3,
  ATTR_BLINK = 1 << 4,
  ATTR_INVERSE = 1 << 5,
  ATTR_INVISIBLE = 1 << 6,
  ATTR_STRIKE = 1 << 7,
};

typedef struct {
  bool set; /* false = terminal default */
  uint8_t r, g, b;
} color_t;

/* A composited cell. `len` bytes of UTF-8; a grapheme cluster may be several
 * codepoints, hence the buffer rather than a codepoint. width 0 means this is
 * the tail half of a wide cell and must not be painted.
 *
 * A shader may write fg, bg and attrs. It may not write text, len or width:
 * selection and copy read the terminal rather than the screen, so rewriting
 * text here would desync them, and changing width would break the invariant
 * the compositor relies on. */
typedef struct {
  char text[16];
  uint8_t len;
  uint8_t width;
  uint16_t attrs;
  color_t fg, bg;
} cell_t;

/* Where a cell sits, and what else the effect is allowed to know. */
typedef struct {
  uint16_t x, y;       /* cell position within the content rect, 0-based */
  uint16_t cols, rows; /* content size, so an effect can be positional */
  int64_t now_ms;      /* for anything animated */
  /* How long this pane has been in the state it is in, in milliseconds — the
   * bell that just rang, the pane that died a moment ago, the one you have not
   * been in for a while. `now_ms` says what time it is; a one-shot effect needs
   * to know how long ago something happened, and only the session can say. 0
   * when there is nothing to date the state from. */
  int64_t state_ms;
  bool focused;

  /* Where the cursor is, in the same rect-relative space as x/y. Only ever
   * set for the pane that owns it, so an effect that follows the cursor does
   * not chase another pane's. */
  bool has_cursor;
  uint16_t cursor_x, cursor_y;

  /* Scrollback, as the viewport sees it: lines hidden above its top edge and
   * below its bottom one. Both 0 when the pane is showing the present, and
   * for a pass over anything that is not a pane's contents. */
  uint32_t above, below;

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

/* Opaque to a shader: a compiled amount expression, owned by the config. When
 * one is attached the pass computes `amount` per cell before calling the
 * shader, so an effect never has to know whether its strength was a number in
 * a config file or a function of where the cell is. */
typedef struct expr_prog expr_prog_t;

struct shader {
  const char *kind; /* registry name, NULL for an empty slot */
  shade_fn fn;
  expr_prog_t *amount_expr; /* NULL when `amount` is just a number */
  color_t color;            /* the target colour, for shaders that have one */
  /* Strength, 0..255; 0 is identity, 255 is fully applied. A cell whose
   * strength works out to 0 is skipped by the pass rather than handed to the
   * shader, so a shader is never called to do nothing -- and a cell the
   * terminal was drawing in its own default colour stays that way. */
  uint8_t amount;
  /* One number whose meaning is the shader's own, because a second parameter
   * that is a column for one effect and a radius for another is not really
   * two things:
   *   ruler     the column to mark          margin  first column to dim
   *   zebra     rows per band               spotlight  radius in columns
   *   gradient  0 down, 1 up, 2 right, 3 left
   */
  uint16_t param;
  /* SHADE_FG / SHADE_BG / SHADE_BOTH, and 0 for both. Opaque to a shader, like
   * `amount_expr` above: it is the pass that keeps a channel, so a shader
   * cannot get it wrong and does not have to know. */
  uint8_t channels;
};

/* A named shader: what the registry stores. */
typedef struct {
  const char *name; /* what to call it in the config */
  shade_fn fn;
} shader_def_t;

#endif /* SLOSH_SHADER_ABI_H */
