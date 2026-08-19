/* Fuzz target: the shader expression language (src/expr.c).
 *
 * Compiles arbitrary text, and when a program comes back actually runs it:
 * once per hand-picked environment (zeros, a plausible pane, and extremes)
 * and once as an amount map over a small rect. The language is documented as
 * total -- no way to hang, div/mod by zero is 0 -- so anything ASan/UBSan
 * catches in eval is a real bug, including signed overflow in the arithmetic.
 */
#include "expr.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *text = malloc(size + 1);
  if (!text) return 0;
  memcpy(text, data, size);
  text[size] = '\0';

  char err[256];
  expr_prog_t *p = expr_compile(text, err, sizeof err);
  if (p) {
    unsigned deps = expr_deps(p);
    if (deps == 0) (void)expr_constant(p);
    (void)expr_source(p);

    const expr_env_t envs[] = {
        {0},
        {.x = 79,
         .y = 23,
         .cols = 80,
         .rows = 24,
         .curx = 40,
         .cury = 12,
         .cursor = 1,
         .focused = 1,
         .t = 1234567890123LL,
         .since = 42,
         .above = 10000,
         .below = 3},
        {.x = INT_MAX,
         .y = INT_MIN,
         .cols = INT_MAX,
         .rows = INT_MAX,
         .curx = -1,
         .cury = -1,
         .cursor = -1,
         .focused = -1,
         .t = INT64_MAX,
         .since = INT64_MIN,
         .above = INT_MAX,
         .below = INT_MIN},
    };
    for (size_t i = 0; i < sizeof envs / sizeof envs[0]; i++)
      (void)expr_eval(p, &envs[i]);

    /* the cached per-cell map, over a rect small enough to fill twice */
    expr_env_t env = {.cols = 8, .rows = 4, .cursor = 1, .t = 17};
    uint8_t *map = NULL;
    (void)expr_amount_map(p, &env, &map);
    env.curx = 3;
    env.cury = 2;
    (void)expr_amount_map(p, &env, &map); /* map is owned by the program */

    expr_free(p);
  }
  free(text);
  return 0;
}
