/* The shader ABI: everything a shader needs to see, and nothing else.
 *
 * This header exists so a shader can be compiled *outside* this tree — a
 * plugin includes only this file, with no libghostty-vt, no pty, no layout.
 * That is also the contract: a shader gets a cell and where it sits, and it
 * cannot reach anything else, because nothing else is declared here.
 *
 * Everything in here is ABI. Changing the layout of these structs breaks
 * every plugin built against the old one, so SL0PPTY_SHADER_ABI is bumped
 * when they change, and a plugin that reports the wrong version (or the wrong
 * sizes, which catches a plugin built against an edited copy of this file) is
 * refused at load rather than trusted into a crash.
 */
#ifndef SL0PPTY_SHADER_ABI_H
#define SL0PPTY_SHADER_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL0PPTY_SHADER_ABI 1

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

/* ---- plugins ------------------------------------------------------------
 *
 * A shared library dropped in the shader directory, exporting one symbol:
 *
 *   const shader_plugin_t *sl0ppty_shader_plugin(void);
 *
 * SL0PPTY_SHADER_PLUGIN() writes that for you. See
 * contrib/shader-plugin/ for a complete one.
 */

typedef struct {
  const char *name; /* what to call it in the config */
  shade_fn fn;
} shader_def_t;

typedef struct {
  uint32_t abi;         /* SL0PPTY_SHADER_ABI it was built against */
  uint32_t cell_size;   /* the three sizes catch a plugin built against a */
  uint32_t ctx_size;    /* copy of this header that has drifted, which a */
  uint32_t shader_size; /* version number alone would not */
  const char *name;     /* the bundle's name, for the log line */
  size_t count;
  const shader_def_t *shaders;
} shader_plugin_t;

#define SL0PPTY_SHADER_PLUGIN_SYM "sl0ppty_shader_plugin"

/* Define the entry point. `defs` is a static array of shader_def_t. */
#define SL0PPTY_SHADER_PLUGIN(bundle_name, defs)                          \
  const shader_plugin_t *sl0ppty_shader_plugin(void);                     \
  const shader_plugin_t *sl0ppty_shader_plugin(void) {                    \
    static const shader_plugin_t table = {                                \
        .abi = SL0PPTY_SHADER_ABI,                                        \
        .cell_size = sizeof(cell_t),                                      \
        .ctx_size = sizeof(shade_ctx_t),                                  \
        .shader_size = sizeof(shader_t),                                  \
        .name = (bundle_name),                                            \
        .count = sizeof(defs) / sizeof((defs)[0]),                        \
        .shaders = (defs),                                                \
    };                                                                    \
    return &table;                                                        \
  }

#endif /* SL0PPTY_SHADER_ABI_H */
