/* Evaluate shader expressions with the real compiler, for tests that need to
 * compare it against something else (contrib/shadertoy.html reimplements the
 * language in JavaScript, and a preview that lies is worse than none).
 *
 * One request per line on stdin:
 *
 *   <x> <y> <cols> <rows> <curx> <cury> <cursor> <focused> <t> <since>
 *   \t<expression>
 *
 * One answer per line on stdout: the value, or `error: ...`.
 */
#define _GNU_SOURCE
#include "expr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char line[4096];
  while (fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0]) continue;

    char *tab = strchr(line, '\t');
    if (!tab) {
      printf("error: no expression\n");
      continue;
    }
    *tab = 0;

    expr_env_t env = {0};
    long long t = 0, since = 0;
    if (sscanf(line, "%d %d %d %d %d %d %d %d %lld %lld", &env.x, &env.y,
               &env.cols, &env.rows, &env.curx, &env.cury, &env.cursor,
               &env.focused, &t, &since) != 10) {
      printf("error: bad environment\n");
      continue;
    }
    env.t = t;
    env.since = since;

    char err[128] = {0};
    expr_prog_t *p = expr_compile(tab + 1, err, sizeof err);
    if (!p) {
      printf("error: %s\n", err[0] ? err : "refused");
      continue;
    }
    printf("%d\n", expr_eval(p, &env));
    expr_free(p);
  }
  return 0;
}
