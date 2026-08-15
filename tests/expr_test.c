/* The shader expression compiler and its VM. Pure text in -> numbers out, so
 * pure tests: no screen, no terminal, no timing. */
#include "expr.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
static char detail[256];

static void ok(const char *name, bool cond, const char *what) {
  if (!cond) fails++;
  printf("%s %-56s %s\n", cond ? "ok  " : "FAIL", name, cond ? "" : what);
}

/* Evaluate at a fixed point, so a test says what it means about the maths. */
static int at(const char *src, int x, int y) {
  char err[128] = {0};
  expr_prog_t *p = expr_compile(src, err, sizeof err);
  if (!p) {
    snprintf(detail, sizeof detail, "%s: %s", src, err);
    return -99999;
  }
  expr_env_t env = {.x = x, .y = y, .cols = 80, .rows = 24,
                    .curx = 10, .cury = 5, .cursor = 1, .focused = 1,
                    .t = 1000, .since = 250};
  int v = expr_eval(p, &env);
  expr_free(p);
  return v;
}

static void eq(const char *src, int want) {
  int got = at(src, 3, 4);
  if (got != want) snprintf(detail, sizeof detail, "%s -> %d, wanted %d", src, got, want);
  ok(src, got == want, detail);
}

static void refused(const char *label, const char *src) {
  char err[128] = {0};
  expr_prog_t *p = expr_compile(src, err, sizeof err);
  if (p) expr_free(p);
  snprintf(detail, sizeof detail, "%s compiled", src);
  ok(label, p == NULL && err[0] != 0, detail);
}

int main(void) {
  printf("-- arithmetic and precedence\n");
  eq("1 + 2 * 3", 7);
  eq("(1 + 2) * 3", 9);
  eq("10 - 3 - 2", 5); /* left-associative, not 9 */
  eq("7 % 3", 1);
  eq("-5 + 1", -4);
  eq("100 / 7", 14); /* integer division, truncating */

  printf("\n-- division by zero is defined, not a trap\n");
  eq("5 / 0", 0);
  eq("5 % 0", 0);
  eq("1 + 5 / 0", 1);

  printf("\n-- comparisons and logic produce 0 or 1\n");
  eq("3 < 4", 1);
  eq("4 < 3", 0);
  eq("3 <= 3", 1);
  eq("3 >= 4", 0);
  eq("3 == 3", 1);
  eq("3 != 3", 0);
  eq("1 && 0", 0);
  eq("1 || 0", 1);
  eq("!0", 1);
  eq("!5", 0);
  /* The reason comparisons return numbers: masking is how you write a rule. */
  eq("(3 < 4) * 200", 200);

  printf("\n-- variables\n");
  eq("x", 3);
  eq("y", 4);
  eq("cols", 80);
  eq("rows", 24);
  eq("curx", 10);
  eq("cury", 5);
  eq("cursor", 1);
  eq("focused", 1);
  eq("t", 1000);
  eq("since", 250);
  /* What a flash is: full strength for a moment after the state began, then
   * nothing. `t` cannot express this — it says what time it is, not how long
   * ago something happened. */
  eq("(since < 300) * 255", 255);
  eq("(since < 200) * 255", 0);

  printf("\n-- functions\n");
  eq("min(3, 9)", 3);
  eq("max(3, 9)", 9);
  eq("abs(0 - 7)", 7);
  eq("clamp(300, 0, 255)", 255);
  eq("clamp(-5, 0, 255)", 0);
  eq("clamp(100, 0, 255)", 100);
  /* dist counts a row double, because a cell is about twice as tall as it is
   * wide. Four columns apart is 4; two rows apart is also 4. */
  eq("dist(0, 0, 4, 0)", 4);
  eq("dist(0, 0, 0, 2)", 4);

  printf("\n-- the ternary evaluates both sides and selects\n");
  eq("1 ? 10 : 20", 10);
  eq("0 ? 10 : 20", 20);
  eq("0 ? 1 / 0 : 7", 7); /* the untaken side is safe because nothing traps */

  printf("\n-- an expression that reads nothing is a constant\n");
  {
    char err[128] = {0};
    expr_prog_t *p = expr_compile("2 * 3 + 1", err, sizeof err);
    ok("it compiles", p != NULL, err);
    ok("with no dependencies", p && expr_deps(p) == 0, "deps");
    ok("folded at compile time", p && expr_constant(p) == 7, "value");
    expr_free(p);
  }

  printf("\n-- dependencies are derived from the source, not declared\n");
  {
    struct { const char *src; unsigned want; } cases[] = {
        {"128", 0},
        {"x * 2", EXPR_DEP_POS},
        {"y", EXPR_DEP_POS},
        {"cols / 2", EXPR_DEP_SIZE}, /* not constant: the rect is not known yet */
        {"dist(x, y, curx, cury)", EXPR_DEP_POS | EXPR_DEP_CURSOR},
        {"focused * 90", EXPR_DEP_FOCUS},
        {"t % 1000", EXPR_DEP_TIME},
        {"since", EXPR_DEP_TIME}, /* a clock, so never cached */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
      char err[128] = {0};
      expr_prog_t *p = expr_compile(cases[i].src, err, sizeof err);
      unsigned got = p ? expr_deps(p) : 0xffff;
      snprintf(detail, sizeof detail, "%s -> %u, wanted %u", cases[i].src, got,
               cases[i].want);
      ok(cases[i].src, p && got == cases[i].want, detail);
      expr_free(p);
    }
  }

  printf("\n-- the map is the whole point: computed once, reused\n");
  {
    char err[128] = {0};
    expr_prog_t *p = expr_compile("x + y", err, sizeof err);
    expr_env_t env = {.cols = 8, .rows = 4};
    uint8_t *m1 = NULL, *m2 = NULL;
    ok("a positional program has a map", expr_amount_map(p, &env, &m1), err);
    ok("with the right values", m1 && m1[0] == 0 && m1[8 * 1 + 2] == 3,
       "values");
    ok("asking again returns the same buffer",
       expr_amount_map(p, &env, &m2) && m2 == m1, "buffer");

    /* Resizing must rebuild it: the size is part of what it was computed from,
     * and a map kept across a resize is the stale-state bug this design is
     * supposed to make unrepresentable. */
    env.cols = 4;
    env.rows = 2;
    ok("a different rect rebuilds it",
       expr_amount_map(p, &env, &m2) && m2[4 * 1 + 1] == 2, "rebuild");
    expr_free(p);
  }

  printf("\n-- a clock-reading program has nothing to reuse\n");
  {
    char err[128] = {0};
    expr_prog_t *p = expr_compile("t % 255", err, sizeof err);
    expr_env_t env = {.cols = 4, .rows = 2};
    uint8_t *m = NULL;
    ok("no map is offered", !expr_amount_map(p, &env, &m), "map");
    expr_free(p);
  }

  printf("\n-- the cursor is part of the key\n");
  {
    char err[128] = {0};
    expr_prog_t *p = expr_compile("dist(x, y, curx, cury)", err, sizeof err);
    expr_env_t env = {.cols = 4, .rows = 2, .cursor = 1, .curx = 0, .cury = 0};
    uint8_t *m = NULL;
    expr_amount_map(p, &env, &m);
    int before = m[3];
    env.curx = 3;
    expr_amount_map(p, &env, &m);
    snprintf(detail, sizeof detail, "%d then %d", before, m[3]);
    ok("moving the cursor rebuilds the map", m[3] != before, detail);
    expr_free(p);
  }

  printf("\n-- what is refused, and says why\n");
  refused("an unknown name", "nosuchvar + 1");
  refused("an unknown function", "wobble(1, 2)");
  refused("an unbalanced paren", "(1 + 2");
  refused("a missing operand", "1 +");
  refused("trailing junk", "1 + 2 )");
  refused("an empty expression", "");
  refused("a ternary without its colon", "1 ? 2");
  refused("something that is not an expression at all", "@@@");

  printf("\n-- totality: there is no loop to write\n");
  {
    /* The language has no way to express unbounded work, so this is a
     * statement about the grammar rather than a timing test: a deeply nested
     * expression is bounded by the program length limit and is refused rather
     * than accepted and run forever. */
    char big[4096];
    size_t n = 0;
    for (int i = 0; i < 200; i++) n += (size_t)snprintf(big + n, sizeof big - n, "1+");
    snprintf(big + n, sizeof big - n, "1");
    char err[128] = {0};
    expr_prog_t *p = expr_compile(big, err, sizeof err);
    if (p) expr_free(p);
    ok("an over-long program is refused, not truncated", p == NULL, err);
  }

  printf("\n%s (%d failures)\n", fails ? "FAILED" : "all green", fails);
  return fails ? 1 : 0;
}
