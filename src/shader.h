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
#ifndef SLOSH_SHADER_H
#define SLOSH_SHADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shader_abi.h"
#include "slosh.h"

/* Per pane, and separately per config. Small and fixed: a stack this deep is
 * already a lot of passes over the same cells. */
#define SHADE_MAX 4
/* What one pane can end up running: the ones every pane gets from the config,
 * the ones attached to this pane, and the one or two the session derived for
 * this frame. */
#define SHADE_CHAIN_MAX (SHADE_MAX * 2 + 2)

/* shade_ctx_t, shader_t and shade_fn are the ABI (shader_abi.h). */

/* Build a shader by registry name. False if the name is not known — a
 * built-in, or one a loaded plugin added. */
bool shader_make(shader_t *out, const char *kind, color_t color,
                 uint8_t amount);
/* The same, for the shaders that take a number of their own. */
bool shader_make_p(shader_t *out, const char *kind, color_t color,
                   uint8_t amount, uint16_t param);
/* Iterate the registry: the i'th name, or NULL past the end. Built-ins first,
 * then anything plugins added, in load order. */
const char *shader_kind(size_t i);

/* Load every `*.so` in `dir` that exports a shader plugin, adding what they
 * declare to the registry. Returns how many shaders were added; a directory
 * that does not exist is not an error (it is the normal case). Failures are
 * per file: a library that cannot be opened, does not export the symbol, or
 * was built against another ABI is reported in `err` and skipped, and the
 * rest still load.
 *
 * Idempotent by path: a file already loaded is not loaded again, so calling
 * this on a config reload picks up newly added plugins and disturbs nothing.
 * Nothing is ever unloaded — a shader's function pointer may be sitting in a
 * live config or on a pane, and dlclose() would turn it into a jump into an
 * unmapped page. Replacing a plugin therefore needs a new session.
 *
 * A plugin is native code with the privileges of the session that loads it.
 * This is deliberately a directory you own, not a config value pointing
 * anywhere: the trust decision is "put a file here". */
size_t shader_load_dir(const char *dir, char *err, size_t errcap);

/* Run `n` shaders in order over `r`, skipping the cells inside `hole` (NULL
 * for none). `base` supplies everything in the context except the per-cell
 * position and the size.
 *
 * The hole is what makes a pane's *frame* a rect: chrome is the pane's rect
 * minus its contents, and an effect has to see the whole frame's coordinates
 * or it could not travel round one — four separate passes over the four sides
 * would each start counting from zero. Skipped cells are not visited at all,
 * so a chrome pass cannot touch a cell the content pass owns. */
void shade_apply(screen_t *s, const shader_t *shaders, size_t n, rect_t r,
                 const rect_t *hole, const shade_ctx_t *base);

#endif /* SLOSH_SHADER_H */
