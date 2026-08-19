/* The shader expression compiler and its VM. See expr.h for the model. */
#define _GNU_SOURCE
#include "expr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bytecode ----------------------------------------------------------- */

enum {
  OP_PUSH, /* imm */
  OP_VAR,  /* imm = which */
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_MOD,
  OP_NEG,
  OP_LT,
  OP_GT,
  OP_LE,
  OP_GE,
  OP_EQ,
  OP_NE,
  OP_AND,
  OP_OR,
  OP_NOT,
  /* On the 32-bit value, with JavaScript's semantics to the letter: the shift
   * count is masked to five bits and `>>` keeps the sign. Not a free choice --
   * contrib/shadertoy.html reimplements this language in JS and a test compares
   * the two, so `x << 33` has to mean the same thing in both. */
  OP_BAND,
  OP_BXOR,
  OP_BOR,
  OP_BNOT,
  OP_SHL,
  OP_SHR,
  OP_MIN,
  OP_MAX,
  OP_ABS,
  OP_CLAMP,
  OP_DIST,
  OP_SIN,
  OP_COS,
  OP_SELECT, /* cond a b -> a or b, both already evaluated */
};

/* Variable slots, in the order the names table lists them. */
enum {
  V_X,
  V_Y,
  V_COLS,
  V_ROWS,
  V_CURX,
  V_CURY,
  V_CURSOR,
  V_FOCUSED,
  V_T,
  V_SINCE,
  V_ABOVE,
  V_BELOW
};

/* Angles are counted in degrees here (see `isin`), and pi is not a number in an
 * angle language -- it is a half turn. So `PI` is 180 and `TAU` is 360, and a
 * formula written the radian way ports across unchanged: `sin(TAU * x / cols)` is
 * one cycle across the pane, `PI / 2` is a quarter turn, `PI / 6` is thirty
 * degrees. It is also *more* exact than radians can be in integers, where
 * 3.14159 / 6 is 0 and the common fractions of a turn are whole degrees.
 *
 * Which is why there is no `deg2rad` or `rad2deg` beside them. There is one angle
 * unit and these are two of its landmarks; a conversion would have to either lose
 * the angle (`deg2rad(90)` is 1, and `sin(1)` is nearly nothing) or invent a
 * second scale for the language to disagree with itself about.
 *
 * Spelled-out numbers rather than variables: they read no environment, carry no
 * dependency, and fold into the program like the literals they are. */
static const struct {
  const char *name;
  int32_t value;
} CONSTS[] = {
    {"PI", 180},  /* a half turn */
    {"TAU", 360}, /* ...and a whole one */
};

static const struct {
  const char *name;
  int slot;
  unsigned dep;
} VARS[] = {
    {"x", V_X, EXPR_DEP_POS},
    {"y", V_Y, EXPR_DEP_POS},
    {"cols", V_COLS, EXPR_DEP_SIZE},
    {"rows", V_ROWS, EXPR_DEP_SIZE},
    {"curx", V_CURX, EXPR_DEP_CURSOR},
    {"cury", V_CURY, EXPR_DEP_CURSOR},
    {"cursor", V_CURSOR, EXPR_DEP_CURSOR},
    {"focused", V_FOCUSED, EXPR_DEP_FOCUS},
    {"t", V_T, EXPR_DEP_TIME},
    /* Also a clock, so it carries the same dependency: never cached, and the
     * session keeps painting while something reads it. */
    {"since", V_SINCE, EXPR_DEP_TIME},
    /* Scrollback, as the viewport sees it: lines hidden above its top edge,
     * and below its bottom one. Both 0 at the present. A dependency of their
     * own rather than a clock: they change per scroll event, not per frame,
     * so the map is reused while the position holds still. */
    {"above", V_ABOVE, EXPR_DEP_SCROLL},
    {"below", V_BELOW, EXPR_DEP_SCROLL},
};

/* cols/rows are a dependency even though the map's key always carries the
 * rect: what the flag prevents is *folding* them at compile time, where the
 * rect is not known and every size reads as zero. Found by a unit test that
 * asked what `cols` evaluates to. */

typedef struct {
  uint8_t op;
  int32_t imm;
} insn_t;

#define MAX_INSNS 256
#define MAX_STACK 32

struct expr_prog {
  insn_t code[MAX_INSNS];
  size_t n;
  unsigned deps;
  bool constant;
  int value; /* when constant */

  /* The memo: the last map this program produced, and what it was produced
   * from. Held here rather than by the caller because a program is compiled
   * once and shared by every copy of the shader that uses it, so this is the
   * one place that outlives a frame. The key is every input the compiler says
   * the program reads, which is what makes a stale map unrepresentable rather
   * than merely unlikely. */
  char *src; /* what was compiled, for the dump to render back out */
  uint8_t *map;
  size_t map_cap;
  bool map_valid;
  int k_cols, k_rows, k_curx, k_cury, k_cursor, k_focused;
  int k_above, k_below;
};

/* ---- lexer -------------------------------------------------------------- */

typedef struct {
  const char *p;
  const char *start;
  char *err;
  size_t errcap;
  bool failed;
  int depth;
} parser_t;

#define MAX_DEPTH 256

static void fail(parser_t *ps, const char *what) {
  if (ps->failed) return;
  ps->failed = true;
  if (ps->err && ps->errcap)
    snprintf(ps->err, ps->errcap, "%s at offset %d", what,
             (int)(ps->p - ps->start));
}

static void skip_ws(parser_t *ps) {
  while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
    ps->p++;
}

static bool eat(parser_t *ps, const char *tok) {
  skip_ws(ps);
  size_t n = strlen(tok);
  if (strncmp(ps->p, tok, n) != 0) return false;
  /* `<` must not eat the `<` of `<=`; callers try the longer one first, but
   * an identifier-like token must also not match a prefix of a longer name. */
  ps->p += n;
  return true;
}

static void emit(parser_t *ps, expr_prog_t *pr, uint8_t op, int32_t imm) {
  if (pr->n >= MAX_INSNS) {
    fail(ps, "expression too long");
    return;
  }
  pr->code[pr->n++] = (insn_t){op, imm};
}

/* ---- parser (precedence climbing) --------------------------------------- */

static void parse_expr(parser_t *ps, expr_prog_t *pr);

static void parse_primary(parser_t *ps, expr_prog_t *pr) {
  skip_ws(ps);
  if (ps->failed) return;

  if (*ps->p == '(') {
    if (++ps->depth > MAX_DEPTH) {
      fail(ps, "expression nested too deeply");
      return;
    }
    ps->p++;
    parse_expr(ps, pr);
    skip_ws(ps);
    ps->depth--;
    if (*ps->p != ')') {
      fail(ps, "expected )");
      return;
    }
    ps->p++;
    return;
  }

  if (*ps->p == '-') {
    ps->p++;
    parse_primary(ps, pr);
    emit(ps, pr, OP_NEG, 0);
    return;
  }
  if (*ps->p == '~') {
    ps->p++;
    parse_primary(ps, pr);
    emit(ps, pr, OP_BNOT, 0);
    return;
  }
  if (*ps->p == '!') {
    ps->p++;
    parse_primary(ps, pr);
    emit(ps, pr, OP_NOT, 0);
    return;
  }

  if (*ps->p >= '0' && *ps->p <= '9') {
    long v = strtol(ps->p, (char **)&ps->p, 10);
    if (v > INT32_MAX) v = INT32_MAX;
    emit(ps, pr, OP_PUSH, (int32_t)v);
    return;
  }

  if ((*ps->p >= 'a' && *ps->p <= 'z') || (*ps->p >= 'A' && *ps->p <= 'Z') ||
      *ps->p == '_') {
    const char *s = ps->p;
    while ((*ps->p >= 'a' && *ps->p <= 'z') ||
           (*ps->p >= 'A' && *ps->p <= 'Z') ||
           (*ps->p >= '0' && *ps->p <= '9') || *ps->p == '_')
      ps->p++;
    size_t len = (size_t)(ps->p - s);
    char name[32];
    if (len >= sizeof name) {
      fail(ps, "name too long");
      return;
    }
    memcpy(name, s, len);
    name[len] = 0;

    skip_ws(ps);
    if (*ps->p == '(') { /* a call */
      ps->p++;
      static const struct {
        const char *name;
        int args;
        uint8_t op;
      } FNS[] = {
          {"min", 2, OP_MIN},     {"max", 2, OP_MAX},   {"abs", 1, OP_ABS},
          {"clamp", 3, OP_CLAMP}, {"dist", 4, OP_DIST}, {"sin", 1, OP_SIN},
          {"cos", 1, OP_COS},
      };
      for (size_t i = 0; i < sizeof FNS / sizeof *FNS; i++) {
        if (strcmp(FNS[i].name, name) != 0) continue;
        for (int a = 0; a < FNS[i].args; a++) {
          if (a) {
            skip_ws(ps);
            if (*ps->p != ',') {
              fail(ps, "expected ,");
              return;
            }
            ps->p++;
          }
          parse_expr(ps, pr);
          if (ps->failed) return;
        }
        skip_ws(ps);
        if (*ps->p != ')') {
          fail(ps, "expected )");
          return;
        }
        ps->p++;
        emit(ps, pr, FNS[i].op, 0);
        return;
      }
      fail(ps, "unknown function");
      return;
    }

    for (size_t i = 0; i < sizeof CONSTS / sizeof *CONSTS; i++) {
      if (strcmp(CONSTS[i].name, name) != 0) continue;
      emit(ps, pr, OP_PUSH, CONSTS[i].value);
      return;
    }
    for (size_t i = 0; i < sizeof VARS / sizeof *VARS; i++) {
      if (strcmp(VARS[i].name, name) != 0) continue;
      pr->deps |= VARS[i].dep;
      emit(ps, pr, OP_VAR, VARS[i].slot);
      return;
    }
    fail(ps, "unknown name");
    return;
  }

  fail(ps, "expected a value");
}

static void parse_mul(parser_t *ps, expr_prog_t *pr) {
  parse_primary(ps, pr);
  for (;;) {
    if (ps->failed) return;
    skip_ws(ps);
    uint8_t op;
    if (*ps->p == '*')
      op = OP_MUL;
    else if (*ps->p == '/')
      op = OP_DIV;
    else if (*ps->p == '%')
      op = OP_MOD;
    else
      return;
    ps->p++;
    parse_primary(ps, pr);
    emit(ps, pr, op, 0);
  }
}

static void parse_add(parser_t *ps, expr_prog_t *pr) {
  parse_mul(ps, pr);
  for (;;) {
    if (ps->failed) return;
    skip_ws(ps);
    uint8_t op;
    if (*ps->p == '+')
      op = OP_ADD;
    else if (*ps->p == '-')
      op = OP_SUB;
    else
      return;
    ps->p++;
    parse_mul(ps, pr);
    emit(ps, pr, op, 0);
  }
}

/* Bitwise, between arithmetic and comparison.
 *
 * C puts `&` `^` `|` *below* comparison, so `x & 7 == 0` there means
 * `x & (7 == 0)`. Ritchie called that a mistake and it is one; a small language
 * copying it would be inheriting a wart for the sake of familiarity, so here the
 * bits bind tighter and the line asks what it looks like it asks. Shifts sit
 * where C puts them, just under `+ -`. */
static void parse_shift(parser_t *ps, expr_prog_t *pr) {
  parse_add(ps, pr);
  for (;;) {
    if (ps->failed) return;
    skip_ws(ps);
    uint8_t op;
    if (eat(ps, "<<"))
      op = OP_SHL;
    else if (eat(ps, ">>"))
      op = OP_SHR;
    else
      return;
    parse_add(ps, pr);
    emit(ps, pr, op, 0);
  }
}

static void parse_band(parser_t *ps, expr_prog_t *pr) {
  parse_shift(ps, pr);
  for (;;) {
    if (ps->failed) return;
    skip_ws(ps);
    /* Not the `&` of `&&`: that one belongs to the logical rung above. */
    if (*ps->p != '&' || ps->p[1] == '&') return;
    ps->p++;
    parse_shift(ps, pr);
    emit(ps, pr, OP_BAND, 0);
  }
}

static void parse_bxor(parser_t *ps, expr_prog_t *pr) {
  parse_band(ps, pr);
  for (;;) {
    if (ps->failed) return;
    skip_ws(ps);
    if (*ps->p != '^') return;
    ps->p++;
    parse_band(ps, pr);
    emit(ps, pr, OP_BXOR, 0);
  }
}

static void parse_bor(parser_t *ps, expr_prog_t *pr) {
  parse_bxor(ps, pr);
  for (;;) {
    if (ps->failed) return;
    skip_ws(ps);
    if (*ps->p != '|' || ps->p[1] == '|') return;
    ps->p++;
    parse_bxor(ps, pr);
    emit(ps, pr, OP_BOR, 0);
  }
}

static void parse_cmp(parser_t *ps, expr_prog_t *pr) {
  parse_bor(ps, pr);
  for (;;) {
    if (ps->failed) return;
    skip_ws(ps);
    uint8_t op;
    /* Longest first: `<` would otherwise eat the `<` of `<=` and leave an `=`
     * that parses as nothing. */
    if (eat(ps, "<="))
      op = OP_LE;
    else if (eat(ps, ">="))
      op = OP_GE;
    else if (eat(ps, "=="))
      op = OP_EQ;
    else if (eat(ps, "!="))
      op = OP_NE;
    else if (*ps->p == '<') {
      ps->p++;
      op = OP_LT;
    } else if (*ps->p == '>') {
      ps->p++;
      op = OP_GT;
    } else
      return;
    parse_bor(ps, pr);
    emit(ps, pr, op, 0);
  }
}

static void parse_and(parser_t *ps, expr_prog_t *pr) {
  parse_cmp(ps, pr);
  while (!ps->failed && (skip_ws(ps), eat(ps, "&&"))) {
    parse_cmp(ps, pr);
    emit(ps, pr, OP_AND, 0);
  }
}

static void parse_or(parser_t *ps, expr_prog_t *pr) {
  parse_and(ps, pr);
  while (!ps->failed && (skip_ws(ps), eat(ps, "||"))) {
    parse_and(ps, pr);
    emit(ps, pr, OP_OR, 0);
  }
}

/* `c ? a : b` evaluates all three and selects. There are no side effects to
 * skip, and no jumps means the VM stays a straight walk over the code, which
 * is what keeps evaluation O(program length) and the language total. */
static void parse_expr(parser_t *ps, expr_prog_t *pr) {
  parse_or(ps, pr);
  if (ps->failed) return;
  skip_ws(ps);
  if (*ps->p != '?') return;
  ps->p++;
  parse_expr(ps, pr);
  skip_ws(ps);
  if (*ps->p != ':') {
    fail(ps, "expected :");
    return;
  }
  ps->p++;
  parse_expr(ps, pr);
  emit(ps, pr, OP_SELECT, 0);
}

/* ---- trigonometry, in the only arithmetic this language has ---------------
 *
 * `sin(d)` and `cos(d)` take **degrees** and answer -255..255. Degrees because
 * they are what a person writing a config guesses, and 255 because that is the
 * strength scale everything else works in: `128 + sin(t / 4) / 2` is a breathe
 * centred half way up, with nothing to rescale.
 *
 * A quarter turn of round(255*sin) at one-degree steps, with the other three
 * quadrants by symmetry. No interpolation: the argument is an integer and there
 * is nothing between two degrees to interpolate. A table rather than a
 * polynomial for one reason -- contrib/shadertoy.html has to answer identically,
 * and identical data with identical reduction cannot drift the way two
 * approximations of the same curve do.
 */
static const int16_t SIN_Q[91] = {
    0,   4,   9,   13,  18,  22,  27,  31,  35,  40,  44,  49,  53,
    57,  62,  66,  70,  75,  79,  83,  87,  91,  96,  100, 104, 108,
    112, 116, 120, 124, 127, 131, 135, 139, 143, 146, 150, 153, 157,
    160, 164, 167, 171, 174, 177, 180, 183, 186, 190, 192, 195, 198,
    201, 204, 206, 209, 211, 214, 216, 219, 221, 223, 225, 227, 229,
    231, 233, 235, 236, 238, 240, 241, 243, 244, 245, 246, 247, 248,
    249, 250, 251, 252, 253, 253, 254, 254, 254, 255, 255, 255, 255,
};

static int32_t isin(int32_t deg) {
  int32_t d = deg % 360;
  if (d < 0) d += 360;
  if (d <= 90) return SIN_Q[d];
  if (d <= 180) return SIN_Q[180 - d];
  if (d <= 270) return -SIN_Q[d - 180];
  return -SIN_Q[360 - d];
}

/* ---- VM ----------------------------------------------------------------- */

static int vm_run(const expr_prog_t *pr, const expr_env_t *env) {
  int32_t st[MAX_STACK];
  int sp = 0;
  const int32_t vars[] = {
      [V_X] = env->x,
      [V_Y] = env->y,
      [V_COLS] = env->cols,
      [V_ROWS] = env->rows,
      [V_CURX] = env->curx,
      [V_CURY] = env->cury,
      [V_CURSOR] = env->cursor,
      [V_FOCUSED] = env->focused,
      [V_T] = (int32_t)(env->t & 0x7fffffff),
      /* Clamped, not wrapped: a pane that has been unfocused for a month
       * should read as "a long time", and a `since` that rolled over would
       * make an effect restart for no reason anybody could see. */
      [V_SINCE] = (int32_t)(env->since < 0            ? 0
                            : env->since > 0x7fffffff ? 0x7fffffff
                                                      : env->since),
      [V_ABOVE] = env->above,
      [V_BELOW] = env->below,
  };

  for (size_t i = 0; i < pr->n; i++) {
    const insn_t *in = &pr->code[i];
    /* The compiler bounds the stack (see expr_compile), so an overflow here
     * would be a compiler bug rather than a program's doing; the guard costs
     * a predictable branch and turns that into a wrong colour, not a smash. */
    if (sp >= MAX_STACK - 1) return 0;
    switch (in->op) {
    case OP_PUSH: st[sp++] = in->imm; break;
    case OP_VAR: st[sp++] = vars[in->imm]; break;
    /* Negation wraps: -INT32_MIN is itself, as `x|0` maths in the JS
     * preview would have it, rather than the undefined overflow it is in C. */
    case OP_NEG:
      if (sp) st[sp - 1] = (int32_t)(0u - (uint32_t)st[sp - 1]);
      break;
    case OP_NOT:
      if (sp) st[sp - 1] = !st[sp - 1];
      break;
    case OP_BNOT:
      if (sp) st[sp - 1] = (int32_t)~(uint32_t)st[sp - 1];
      break;
    case OP_ABS:
      if (sp && st[sp - 1] < 0)
        st[sp - 1] = (int32_t)(0u - (uint32_t)st[sp - 1]);
      break;
    case OP_SIN:
      if (sp) st[sp - 1] = isin(st[sp - 1]);
      break;
    case OP_COS:
      if (sp) st[sp - 1] = isin((int32_t)((uint32_t)st[sp - 1] + 90u));
      break;
    default: {
      int need = in->op == OP_CLAMP    ? 3
                 : in->op == OP_DIST   ? 4
                 : in->op == OP_SELECT ? 3
                                       : 2;
      if (sp < need) return 0;
      int32_t b = st[sp - 1], a = st[sp - 2];
      switch (in->op) {
      /* Arithmetic wraps as two's-complement int32, like `|0` in JS and
           * unlike C, where signed overflow is undefined: `t` only grows, so
           * a long-lived session's animation must not be able to walk an
           * expression into UB. Done unsigned, where wrapping is defined. */
      case OP_ADD:
        st[sp - 2] = (int32_t)((uint32_t)a + (uint32_t)b);
        sp--;
        break;
      case OP_SUB:
        st[sp - 2] = (int32_t)((uint32_t)a - (uint32_t)b);
        sp--;
        break;
      case OP_MUL:
        st[sp - 2] = (int32_t)((uint32_t)a * (uint32_t)b);
        sp--;
        break;
      /* Defined rather than trapping: an expression that divides by zero
           * should look wrong, not take the session with it. INT32_MIN / -1
           * is the one other trapping division (SIGFPE on x86); it wraps to
           * INT32_MIN like the other arithmetic, and the matching modulo
           * is 0. */
      case OP_DIV:
        st[sp - 2] = !b ? 0 : (a == INT32_MIN && b == -1) ? a : a / b;
        sp--;
        break;
      case OP_MOD:
        st[sp - 2] = !b ? 0 : (a == INT32_MIN && b == -1) ? 0 : a % b;
        sp--;
        break;
      case OP_LT:
        st[sp - 2] = a < b;
        sp--;
        break;
      case OP_GT:
        st[sp - 2] = a > b;
        sp--;
        break;
      case OP_LE:
        st[sp - 2] = a <= b;
        sp--;
        break;
      case OP_GE:
        st[sp - 2] = a >= b;
        sp--;
        break;
      case OP_EQ:
        st[sp - 2] = a == b;
        sp--;
        break;
      case OP_NE:
        st[sp - 2] = a != b;
        sp--;
        break;
      case OP_AND:
        st[sp - 2] = a && b;
        sp--;
        break;
      case OP_BAND:
        st[sp - 2] = (int32_t)((uint32_t)a & (uint32_t)b);
        sp--;
        break;
      case OP_BXOR:
        st[sp - 2] = (int32_t)((uint32_t)a ^ (uint32_t)b);
        sp--;
        break;
      case OP_BOR:
        st[sp - 2] = (int32_t)((uint32_t)a | (uint32_t)b);
        sp--;
        break;
      /* Count masked to five bits, like JS, so a silly shift is defined
           * rather than undefined. Shifted unsigned because moving a bit into
           * the sign of a signed value is undefined in C and simply wraps in JS,
           * which is what the preview will show. */
      case OP_SHL:
        st[sp - 2] = (int32_t)((uint32_t)a << (b & 31));
        sp--;
        break;
      /* Arithmetic: the sign is kept, so `-8 >> 1` is -4. Written out rather
           * than left to the compiler, because a signed right shift is
           * implementation-defined in C and this one has to match JS. */
      case OP_SHR: {
        unsigned k = (unsigned)(b & 31);
        uint32_t u = (uint32_t)a;
        st[sp - 2] = (int32_t)(a < 0 ? ~((~u) >> k) : u >> k);
        sp--;
        break;
      }
      case OP_OR:
        st[sp - 2] = a || b;
        sp--;
        break;
      case OP_MIN:
        st[sp - 2] = a < b ? a : b;
        sp--;
        break;
      case OP_MAX:
        st[sp - 2] = a > b ? a : b;
        sp--;
        break;
      case OP_CLAMP: {
        int32_t hi = st[sp - 1], lo = st[sp - 2], v = st[sp - 3];
        st[sp - 3] = v < lo ? lo : v > hi ? hi : v;
        sp -= 2;
        break;
      }
      case OP_SELECT: {
        int32_t f = st[sp - 1], t = st[sp - 2], c = st[sp - 3];
        st[sp - 3] = c ? t : f;
        sp -= 2;
        break;
      }
      case OP_DIST: {
        /* A cell is about twice as tall as it is wide, so a distance in
             * cells counts a row double -- the same correction the layout's
             * own centre-distance makes. Without it every "circle" here is an
             * ellipse, which is the first thing anyone notices. */
        int32_t y2 = st[sp - 1], x2 = st[sp - 2], y1 = st[sp - 3],
                x1 = st[sp - 4];
        /* 64-bit, because the operands are arbitrary int32 and squaring a
             * difference of those overflows any int32 with room to spare --
             * and the components saturate at 2^16, because the result is
             * capped at 4096 anyway and a doubled int32 difference squared
             * would overflow even 64 bits. Unobservable except by not
             * crashing. */
        int64_t dx = (int64_t)x1 - x2, dy = ((int64_t)y1 - y2) * 2;
        if (dx > 65536) dx = 65536;
        if (dx < -65536) dx = -65536;
        if (dy > 65536) dy = 65536;
        if (dy < -65536) dy = -65536;
        int64_t d2 = dx * dx + dy * dy;
        /* Integer square root, so `dist` is a distance and not its
             * square: expressions that scale linearly with it are what people
             * write, and squaring is easy to ask for and hard to undo. */
        int64_t r = 0;
        while ((r + 1) * (r + 1) <= d2 && r < 4096) r++;
        st[sp - 4] = (int32_t)r;
        sp -= 3;
        break;
      }
      default: return 0;
      }
      break;
    }
    }
  }
  return sp ? st[sp - 1] : 0;
}

/* ---- compile ------------------------------------------------------------ */

/* The deepest the stack can get, which is what makes MAX_STACK a fact rather
 * than a hope: every opcode's effect on depth is known, so the maximum is a
 * walk over the code. */
static bool stack_fits(const expr_prog_t *pr) {
  int sp = 0, peak = 0;
  for (size_t i = 0; i < pr->n; i++) {
    switch (pr->code[i].op) {
    case OP_PUSH:
    case OP_VAR: sp++; break;
    case OP_NEG:
    case OP_NOT:
    case OP_BNOT:
    case OP_SIN:
    case OP_COS:
    case OP_ABS: break;
    case OP_CLAMP:
    case OP_SELECT: sp -= 2; break;
    case OP_DIST: sp -= 3; break;
    default: sp--; break;
    }
    if (sp > peak) peak = sp;
    if (sp < 0) return false;
  }
  return peak < MAX_STACK - 1 && sp == 1;
}

expr_prog_t *expr_compile(const char *src, char *err, size_t errcap) {
  if (err && errcap) err[0] = 0;
  if (!src || !*src) {
    if (err) snprintf(err, errcap, "empty expression");
    return NULL;
  }

  expr_prog_t *pr = calloc(1, sizeof *pr);
  if (!pr) return NULL;

  parser_t ps = {.p = src, .start = src, .err = err, .errcap = errcap};
  parse_expr(&ps, pr);
  skip_ws(&ps);
  if (!ps.failed && *ps.p) fail(&ps, "unexpected trailing input");
  if (!ps.failed && !stack_fits(pr)) fail(&ps, "malformed expression");

  if (ps.failed) {
    free(pr);
    return NULL;
  }

  /* An expression that reads nothing is a number written the long way, and
   * every later stage is happier not knowing the difference. */
  if (!pr->deps) {
    expr_env_t zero = {0};
    pr->value = vm_run(pr, &zero);
    pr->constant = true;
  }
  /* The source, kept: a program lives in configs that are dumped back out,
   * and bytecode cannot be un-compiled into the line somebody wrote. */
  pr->src = strdup(src);
  return pr;
}

void expr_free(expr_prog_t *p) {
  if (!p) return;
  free(p->src);
  free(p->map);
  free(p);
}

const char *expr_source(const expr_prog_t *p) { return p ? p->src : NULL; }

/* The language's own sine, exported: C-side animation (the splash particles)
 * wants the same table the expressions use, so a motion sketched in the repl
 * and one compiled into the binary cannot disagree about what sin(90) is. */
int expr_sin(int deg) { return (int)isin(deg); }

unsigned expr_deps(const expr_prog_t *p) { return p ? p->deps : 0; }
int expr_constant(const expr_prog_t *p) { return p ? p->value : 0; }

int expr_eval(const expr_prog_t *p, const expr_env_t *env) {
  if (!p) return 0;
  if (p->constant) return p->value;
  return vm_run(p, env);
}

static uint8_t clamp8(int v) {
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

bool expr_amount_map(expr_prog_t *p, const expr_env_t *env, uint8_t **out) {
  if (!p || !out) return false;
  if (p->deps & EXPR_DEP_TIME) return false; /* nothing to reuse */

  size_t cells = (size_t)env->cols * (size_t)env->rows;
  if (!cells) return false;

  bool same = p->map_valid && p->k_cols == env->cols && p->k_rows == env->rows;
  if (same && (p->deps & EXPR_DEP_CURSOR))
    same = p->k_curx == env->curx && p->k_cury == env->cury &&
           p->k_cursor == env->cursor;
  if (same && (p->deps & EXPR_DEP_FOCUS)) same = p->k_focused == env->focused;
  if (same && (p->deps & EXPR_DEP_SCROLL))
    same = p->k_above == env->above && p->k_below == env->below;

  if (!same) {
    if (p->map_cap < cells) {
      uint8_t *grown = realloc(p->map, cells);
      if (!grown) return false;
      p->map = grown;
      p->map_cap = cells;
    }
    expr_env_t e = *env;
    for (int y = 0; y < env->rows; y++) {
      e.y = y;
      for (int x = 0; x < env->cols; x++) {
        e.x = x;
        p->map[(size_t)y * env->cols + x] = clamp8(vm_run(p, &e));
      }
    }
    p->map_valid = true;
    p->k_cols = env->cols;
    p->k_rows = env->rows;
    p->k_curx = env->curx;
    p->k_cury = env->cury;
    p->k_cursor = env->cursor;
    p->k_focused = env->focused;
    p->k_above = env->above;
    p->k_below = env->below;
  }
  *out = p->map;
  return true;
}
