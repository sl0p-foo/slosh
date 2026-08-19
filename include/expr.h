/* Shader expressions: a tiny integer language for the one number a shader
 * takes, compiled to bytecode at config load.
 *
 * The idea is smaller than "shaders in a language". Every built-in shader here
 * is already `colour_op(amount)` where the amount is a pure function of where
 * the cell is:
 *
 *   vignette  amount = f(x, y)        ruler   amount = (x == col) * amount
 *   zebra     amount = f(y)           margin  amount = (x >= col) * amount
 *
 * So the part worth handing to a config is the *amount*, not the colour maths.
 * That keeps the mixing in C where it is fast and correct, gives an expression
 * no way to produce an invalid cell (it produces one byte), and \u2014 because the
 * result is a pure function of position \u2014 lets the whole program be evaluated
 * once into a per-cell map and then reused, which is what makes an interpreter
 * affordable at all. Measured: 56ns/cell interpreted, 2ns/cell from the map.
 *
 * The language is deliberately total. No loops, no recursion, no jumps: even
 * `?:` compiles to evaluating both sides and selecting, which is safe because
 * nothing in here has an effect. Evaluation is therefore O(program length),
 * a bad expression cannot hang a session, and there is nothing to sandbox.
 *
 *   variables   x y cols rows curx cury cursor focused t since above below
 *   operators   + - * / %   < > <= >= == !=   && || !   ?:
 *   functions   min max abs clamp dist
 *
 * `x`/`y` are cell positions inside the rect the pass runs over (a pane's
 * contents, or its frame for a chrome pass), `t` is a millisecond clock for
 * animation, `since` is how many milliseconds the pane has been in the state
 * it is in — which is what a one-shot effect needs, because `t` says what time
 * it is and not how long ago something happened. `above`/`below` are the lines
 * of scrollback hidden past the viewport's top and bottom edges, both 0 at
 * the present — which is what lets an effect say "there is more this way".
 * `dist` corrects for a cell being about twice as tall as it is wide. Division and modulo by zero are 0
 * rather than a trap: an expression that is wrong should look wrong, not take
 * the session with it.
 */
#ifndef SLOSH_EXPR_H
#define SLOSH_EXPR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct expr_prog expr_prog_t;

/* What a program reads, worked out by the compiler rather than declared. This
 * is what decides whether its output can be cached and against what, so it is
 * derived from the source and can never disagree with it. */
enum {
  EXPR_DEP_POS = 1 << 0,    /* x, y */
  EXPR_DEP_CURSOR = 1 << 1, /* curx, cury, cursor */
  EXPR_DEP_TIME = 1 << 2,   /* t: recomputed every frame, never cached */
  EXPR_DEP_FOCUS = 1 << 3,  /* focused */
  /* cols, rows. Not part of the cache key -- the key always carries the rect,
   * because a map is per rect by construction -- but it still has to be a
   * dependency, or a program reading only the size would look constant and be
   * folded at compile time, where the size is not known yet and is zero. */
  EXPR_DEP_SIZE = 1 << 4,
  EXPR_DEP_SCROLL = 1 << 5, /* above, below */
};

/* Compile, or NULL with a reason in `err`. A program that reads nothing is
 * folded to its value at compile time and still returned, so callers have one
 * shape to handle. */
expr_prog_t *expr_compile(const char *src, char *err, size_t errcap);
void expr_free(expr_prog_t *p);

/* What it reads (EXPR_DEP_*). 0 means the result is a constant. */
unsigned expr_deps(const expr_prog_t *p);
/* The value of a program that reads nothing; undefined otherwise. */
int expr_constant(const expr_prog_t *p);

/* Everything a program can see. Filled by the shader pass. */
typedef struct {
  int x, y, cols, rows;
  int curx, cury, cursor;
  int focused;
  int64_t t;
  /* Milliseconds since the pane entered its current state, clamped at 0. */
  int64_t since;
  /* Lines of scrollback hidden above and below the viewport, both 0 at the
   * present (and for a pass over anything that is not a pane's contents). */
  int above, below;
} expr_env_t;

/* Evaluate for one cell. Clamped to 0..255 by the caller, not here: the
 * language has no opinion about what its number is for. */
int expr_eval(const expr_prog_t *p, const expr_env_t *env);

/* The text this program was compiled from, owned by the program. For the
 * config dump, which has to write back the line somebody wrote rather than
 * pretend an expression was a number. */
const char *expr_source(const expr_prog_t *p);

/* The language's sine, for C code that animates: degrees in (any angle
 * wraps), -255..255 out -- the same table `sin()` in an expression uses. */
int expr_sin(int deg);

/* Evaluate for every cell of a cols x rows rect into `out` (cols*rows bytes,
 * clamped to 0..255), reusing the last result when nothing it reads has
 * changed. Returns false when the program reads the clock, which is the one
 * case there is nothing to reuse. */
bool expr_amount_map(expr_prog_t *p, const expr_env_t *env, uint8_t **out);

#endif /* SLOSH_EXPR_H */
