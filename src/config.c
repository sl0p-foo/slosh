#define _GNU_SOURCE
#include "config.h"

#include <ghostty/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "input.h"
#include "kdl.h"

static const struct {
  const char *name;
  action_t action;
} ACTIONS[] = {
    {"split-cols", ACT_SPLIT_COLS},   {"split-rows", ACT_SPLIT_ROWS},
    {"close-pane", ACT_CLOSE_PANE},   {"rerun", ACT_RERUN},
    {"zoom", ACT_ZOOM},
    {"minimize", ACT_MINIMIZE},   {"focus-left", ACT_FOCUS_LEFT},
    {"focus-right", ACT_FOCUS_RIGHT}, {"focus-up", ACT_FOCUS_UP},
    {"focus-down", ACT_FOCUS_DOWN},   {"focus-next", ACT_FOCUS_NEXT},
    {"resize-left", ACT_RESIZE_LEFT}, {"resize-right", ACT_RESIZE_RIGHT},
    {"resize-up", ACT_RESIZE_UP},     {"resize-down", ACT_RESIZE_DOWN},
    {"equalize", ACT_EQUALIZE},
    {"rotate-layout", ACT_ROTATE_LAYOUT},
    {"scroll-up", ACT_SCROLL_UP},     {"scroll-down", ACT_SCROLL_DOWN},
    {"scroll-page-up", ACT_SCROLL_PAGE_UP},
    {"scroll-page-down", ACT_SCROLL_PAGE_DOWN},
    {"scroll-top", ACT_SCROLL_TOP},   {"scroll-bottom", ACT_SCROLL_BOTTOM},
    {"new-tab", ACT_NEW_TAB},         {"next-tab", ACT_NEXT_TAB},
    {"prev-tab", ACT_PREV_TAB},       {"finder", ACT_FINDER},
    {"palette", ACT_PALETTE},
    {"detach", ACT_DETACH},           {"quit", ACT_QUIT},
    {"literal-prefix", ACT_LITERAL_PREFIX},
    {"help", ACT_HELP},
    {"edit-config", ACT_EDIT_CONFIG},
};

static const struct {
  const char *name;
  int key;
} NAMED_KEYS[] = {
    {"left", GHOSTTY_KEY_ARROW_LEFT},   {"right", GHOSTTY_KEY_ARROW_RIGHT},
    {"up", GHOSTTY_KEY_ARROW_UP},       {"down", GHOSTTY_KEY_ARROW_DOWN},
    {"enter", GHOSTTY_KEY_ENTER},       {"tab", GHOSTTY_KEY_TAB},
    {"escape", GHOSTTY_KEY_ESCAPE},     {"space", GHOSTTY_KEY_SPACE},
    {"backspace", GHOSTTY_KEY_BACKSPACE}, {"home", GHOSTTY_KEY_HOME},
    {"end", GHOSTTY_KEY_END},           {"pageup", GHOSTTY_KEY_PAGE_UP},
    {"pagedown", GHOSTTY_KEY_PAGE_DOWN},{"delete", GHOSTTY_KEY_DELETE},
    {"insert", GHOSTTY_KEY_INSERT},     {"backslash", GHOSTTY_KEY_BACKSLASH},
    {"minus", GHOSTTY_KEY_MINUS},       {"slash", GHOSTTY_KEY_SLASH},
    {"comma", GHOSTTY_KEY_COMMA},       {"period", GHOSTTY_KEY_PERIOD},
};

/* A character to a key, and whether typing it needs shift.
 *
 * `?` is shift+slash on the keyboard everybody writing a config has, so
 * `bind "?"` means that -- the character is the thing you press, and the shift
 * is not a separate fact about it. Same for a capital letter: the cheatsheet
 * prints shift+h as "H" precisely because that is what you press, and a config
 * has to be able to say the same thing. */
static int key_from_char(char c, bool *shifted) {
  if (shifted) *shifted = false;
  if (c >= 'a' && c <= 'z') return GHOSTTY_KEY_A + (c - 'a');
  if (c >= 'A' && c <= 'Z') {
    if (shifted) *shifted = true;
    return GHOSTTY_KEY_A + (c - 'A');
  }
  if (c >= '0' && c <= '9') return GHOSTTY_KEY_DIGIT_0 + (c - '0');
  switch (c) {
    case '\\': return GHOSTTY_KEY_BACKSLASH;
    case '-': return GHOSTTY_KEY_MINUS;
    case '=': return GHOSTTY_KEY_EQUAL;
    case '[': return GHOSTTY_KEY_BRACKET_LEFT;
    case ']': return GHOSTTY_KEY_BRACKET_RIGHT;
    case ';': return GHOSTTY_KEY_SEMICOLON;
    case '\'': return GHOSTTY_KEY_QUOTE;
    case ',': return GHOSTTY_KEY_COMMA;
    case '.': return GHOSTTY_KEY_PERIOD;
    case '/': return GHOSTTY_KEY_SLASH;
    case '`': return GHOSTTY_KEY_BACKQUOTE;
    case ' ': return GHOSTTY_KEY_SPACE;
    default: break;
  }
  /* The shifted half of the same keys, which config.kdl has always said were
   * writable and the parser has always refused: `bind "?" "help"` is in the
   * file it ships and in every config `--dump-config` has ever written. */
  if (shifted) *shifted = true;
  switch (c) {
    case '|': return GHOSTTY_KEY_BACKSLASH;
    case '_': return GHOSTTY_KEY_MINUS;
    case '+': return GHOSTTY_KEY_EQUAL;
    case '{': return GHOSTTY_KEY_BRACKET_LEFT;
    case '}': return GHOSTTY_KEY_BRACKET_RIGHT;
    case ':': return GHOSTTY_KEY_SEMICOLON;
    case '"': return GHOSTTY_KEY_QUOTE;
    case '<': return GHOSTTY_KEY_COMMA;
    case '>': return GHOSTTY_KEY_PERIOD;
    case '?': return GHOSTTY_KEY_SLASH;
    case '~': return GHOSTTY_KEY_BACKQUOTE;
    default: break;
  }
  if (shifted) *shifted = false;
  return GHOSTTY_KEY_UNIDENTIFIED;
}

/* The arrows the cheatsheet draws, so a chord copied off the screen parses. */
static const struct {
  const char *glyph;
  int key;
} ARROW_GLYPHS[] = {
    {"\u2190", GHOSTTY_KEY_ARROW_LEFT},  {"\u2192", GHOSTTY_KEY_ARROW_RIGHT},
    {"\u2191", GHOSTTY_KEY_ARROW_UP},    {"\u2193", GHOSTTY_KEY_ARROW_DOWN},
};

/* `implied` comes back true when the shift in `out_mods` came from the character
 * rather than from a modifier the config wrote: `?` is shift+slash, `H` is
 * shift+h. It matters because a terminal without the kitty protocol reports a
 * plain `?` byte with no modifier at all -- the character *is* the shift -- so a
 * binding written that way has to answer both encodings. The shipped default
 * binds `?` twice by hand for exactly this reason; this is that, done once. */
static bool parse_chord_ex(const char *text, int *out_key, uint16_t *out_mods,
                           bool *implied) {
  uint16_t mods = 0;
  const char *p = text;
  if (implied) *implied = false;
  for (;;) {
    if (strncmp(p, "ctrl+", 5) == 0) { mods |= MOD_CTRL; p += 5; }
    else if (strncmp(p, "alt+", 4) == 0) { mods |= MOD_ALT; p += 4; }
    else if (strncmp(p, "shift+", 6) == 0) { mods |= MOD_SHIFT; p += 6; }
    else if (strncmp(p, "super+", 6) == 0) { mods |= MOD_SUPER; p += 6; }
    /* The cheatsheet's own shorthand. It prints `C-a`, `M-x`, `S-tab`, and the
     * whole claim of that sheet is that what it shows is what you would write
     * -- which was false for every chord it printed with a modifier. */
    else if (strncmp(p, "C-", 2) == 0) { mods |= MOD_CTRL; p += 2; }
    else if (strncmp(p, "M-", 2) == 0) { mods |= MOD_ALT; p += 2; }
    else if (strncmp(p, "S-", 2) == 0) { mods |= MOD_SHIFT; p += 2; }
    else break;
  }
  if (!*p) return false;

  for (size_t i = 0; i < sizeof NAMED_KEYS / sizeof *NAMED_KEYS; i++)
    if (strcmp(p, NAMED_KEYS[i].name) == 0) {
      *out_key = NAMED_KEYS[i].key;
      *out_mods = mods;
      return true;
    }
  for (size_t i = 0; i < sizeof ARROW_GLYPHS / sizeof *ARROW_GLYPHS; i++)
    if (strcmp(p, ARROW_GLYPHS[i].glyph) == 0) {
      *out_key = ARROW_GLYPHS[i].key;
      *out_mods = mods;
      return true;
    }

  if (p[1] != 0) return false; /* not a name and not one character */
  bool shifted = false;
  int key = key_from_char(*p, &shifted);
  if (key == GHOSTTY_KEY_UNIDENTIFIED) return false;
  *out_key = key;
  *out_mods = (uint16_t)(mods | (shifted ? MOD_SHIFT : 0));
  /* A capital letter is not in this category: the decoder does report `H` as
   * shift+h on every terminal, so one binding is right. Punctuation is where
   * they disagree. */
  if (implied) *implied = shifted && !(*p >= 'A' && *p <= 'Z');
  return true;
}

bool config_parse_chord(const char *text, int *out_key, uint16_t *out_mods) {
  return parse_chord_ex(text, out_key, out_mods, NULL);
}

static color_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (color_t){true, r, g, b};
}

/* "#rrggbb" */
static bool parse_color(const char *text, color_t *out) {
  if (!text || text[0] != '#' || strlen(text) != 7) return false;
  unsigned r, g, b;
  if (sscanf(text + 1, "%2x%2x%2x", &r, &g, &b) != 3) return false;
  *out = rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
  return true;
}

/* One complaint about the config being read.
 *
 * Every one of these is a line the loader could not honour while the rest of
 * the file applied (D9), so they are collected rather than thrown: a session
 * shows the first, because it has one status line, and `--check` shows all of
 * them, because a linter that stops at the first mistake makes you run it once
 * per mistake.
 *
 * `err` is the caller's out-parameter and keeps its old meaning -- the first
 * message, or untouched when there is none. The file and line come from the
 * loader rather than from each call site: a complaint that cannot say where it
 * happened is a complaint you have to go looking for.
 */
static void complain(config_t *c, char *err, size_t errcap, int line,
                     const char *fmt, ...) {
  char text[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);

  char full[192];
  const char *file = c && c->loading ? c->loading : NULL;
  const char *base = file ? strrchr(file, '/') : NULL;
  if (file && line > 0)
    snprintf(full, sizeof full, "%s:%d: %s", base ? base + 1 : file, line, text);
  else if (file)
    snprintf(full, sizeof full, "%s: %s", base ? base + 1 : file, text);
  else
    snprintf(full, sizeof full, "%s", text);

  if (c && c->nmsgs < CONFIG_MSGS_MAX)
    snprintf(c->msgs[c->nmsgs++], sizeof c->msgs[0], "%s", full);
  if (err && errcap && !err[0]) snprintf(err, errcap, "%s", full);
}

size_t config_messages(const config_t *c, const char **out, size_t max) {
  size_t n = c->nmsgs < max ? c->nmsgs : max;
  for (size_t i = 0; i < n; i++) out[i] = c->msgs[i];
  return n;
}

static const char *const PSTATE_NAMES[PSTATE_COUNT] = {
    "dragging", "drop_hover", "drop_target", "dead", "suspended",
    "bell",     "scrolled",   "unfocused",
};

const char *pane_state_name(pane_state_t s) {
  return s < PSTATE_COUNT ? PSTATE_NAMES[s] : "";
}

/* One shader entry -- `dim amount=90 where="chrome" channel="fg"` -- into a
 * shader_t, whatever is asking. The config asks about the children of a
 * `shaders` block; a pane asks about a line a program sent it in-band. Same
 * function, because "what does this entry mean" has to have one answer: a
 * second parser is a second place for `amount` to drift.
 *
 * `*expr` comes back set when the entry compiled an expression, and belongs to
 * the caller: the config owns the ones a file produced, and a pane owns the ones
 * it was sent, because those live exactly as long as that pane's chain. `why`
 * gets a reason when the entry cannot be honoured, and the entry is dropped
 * rather than run at some guessed strength. */
static bool parse_shader_entry(const kdl_node_t *k, color_t default_color,
                               bool default_chrome, bool *on_chrome,
                               shader_t *out,
                               expr_prog_t **expr, char *why, size_t whycap) {
  *expr = NULL;

  /* Where this pass runs. Dropped rather than defaulted when the word is not
   * one we know, for the same reason a bad `amount` is: the entry says what it
   * wants and we cannot do it, and running it over the contents because "chrom"
   * was a typo would be a surprise nobody asked for. */
  const char *where = kdl_prop(k, "where", default_chrome ? "chrome" : "content");
  *on_chrome = strcmp(where, "chrome") == 0;
  if (!*on_chrome && strcmp(where, "content") != 0) {
    snprintf(why, whycap, "bad where for %s: %s (content or chrome)", k->name,
             where);
    return false;
  }

  /* Which of the cell's two colours it may touch. `fg` is what a *border* flash
   * wants: a frame's background is usually the terminal's own default, and
   * mixing that towards a colour turns a recoloured glyph into a painted
   * rectangle. Refused the same way a bad `where` is, and for the same reason. */
  const char *chan = kdl_prop(k, "channel", "both");
  uint8_t channels = SHADE_BOTH;
  if (strcmp(chan, "fg") == 0) channels = SHADE_FG;
  else if (strcmp(chan, "bg") == 0) channels = SHADE_BG;
  else if (strcmp(chan, "both") != 0) {
    snprintf(why, whycap, "bad channel for %s: %s (fg, bg or both)", k->name,
             chan);
    return false;
  }

  /* `amount` is a number, or an expression that produces one per cell. Same key
   * either way: `amount=90` and `amount="(y % 2) * 40"` are the same idea, one
   * of them constant, and the compiler folds a constant expression back to a
   * number so nothing downstream can tell. */
  expr_prog_t *aexpr = NULL;
  long amount = 128;
  const char *as = kdl_prop(k, "amount", NULL);
  if (as) {
    char *endp = NULL;
    long v = strtol(as, &endp, 10);
    while (endp && (*endp == ' ' || *endp == '\t')) endp++;
    if (endp && !*endp) {
      amount = v;
    } else {
      char eerr[128] = {0};
      aexpr = expr_compile(as, eerr, sizeof eerr);
      if (!aexpr) {
        /* Dropped, not run at its default strength: an expression that did not
         * compile leaves the strength *unknown*, and half-dimming a pane is a
         * worse answer to that than doing nothing and saying why. */
        snprintf(why, whycap, "bad amount for %s: %s", k->name, eerr);
        return false;
      }
    }
  }
  if (amount < 0) amount = 0;
  if (amount > 255) amount = 255;

  /* Every shader that takes a number calls it something different, so accept
   * each name rather than making you remember which belongs to which. */
  long param = kdl_prop_int(k, "at", -1);
  if (param < 0) param = kdl_prop_int(k, "radius", -1);
  if (param < 0) param = kdl_prop_int(k, "band", -1);
  if (param < 0) param = kdl_prop_int(k, "direction", -1);
  if (param < 0) param = 0;
  if (param > 65535) param = 65535;

  color_t col = default_color; /* a sensible default for `ruler` */
  const char *cs = kdl_prop(k, "color", NULL);
  bool bad_colour = cs && !parse_color(cs, &col);

  if (!shader_make_p(out, k->name, col, (uint8_t)amount, (uint16_t)param)) {
    snprintf(why, whycap, "unknown shader: %s", k->name);
    expr_free(aexpr);
    return false;
  }
  out->channels = channels;
  out->amount_expr = aexpr;
  *expr = aexpr;
  /* A bad colour is worth saying and not worth dropping the pass over: the
   * shader runs at the default colour, which is visible, rather than not at
   * all, which is not. */
  if (bad_colour) snprintf(why, whycap, "bad colour for shader %s: %s", k->name, cs);
  return true;
}

/* Reads the children of `node` as shader chains: the ones that run over a pane's
 * contents and the ones that run over its frame, routed by each entry's `where`.
 * Counts are set, not added to: naming a block replaces it. */
static void parse_shader_list(config_t *c, const kdl_node_t *node,
                              shader_t *content, size_t *ncontent,
                              shader_t *chrome, size_t *nchrome, char *err,
                              size_t errcap) {
  *ncontent = 0;
  *nchrome = 0;
  for (size_t i = 0; i < node->nkids; i++) {
    const kdl_node_t *k = node->kids[i];
    if (!k || !k->name) continue;

    bool on_chrome = false;
    shader_t made;
    expr_prog_t *aexpr = NULL;
    char why[160] = {0};
    bool ok = parse_shader_entry(k, c->frame_focus, false, &on_chrome, &made,
                                 &aexpr, why, sizeof why);
    if (why[0]) complain(c, err, errcap, k->line, "%s", why);
    if (!ok) continue;

    shader_t *out = on_chrome ? chrome : content;
    size_t *n = on_chrome ? nchrome : ncontent;
    if (*n >= SHADE_MAX) {
      expr_free(aexpr);
      continue;
    }
    out[(*n)++] = made;
    if (aexpr) {
      c->exprs = realloc(c->exprs, (c->nexprs + 1) * sizeof *c->exprs);
      c->exprs[c->nexprs++] = aexpr; /* the config owns every program */
    }
  }
}

/* A chain from text, for whoever is not a config file: `dim amount=90` or
 * `tint where="chrome" channel="fg" amount="(since<250)*255"`, several separated
 * by `;` or newlines. The same entries a `shaders { }` block holds, parsed by
 * the same function, so what you prototype in a pane is what you can paste into
 * a config.
 *
 * Expressions come back in `exprs` for the caller to free. Returns how many
 * shaders were understood; `err` gets the first reason one was not. */
size_t config_parse_chain(const char *text, color_t default_color, bool chrome,
                          shader_t *out, size_t max, expr_prog_t **exprs,
                          size_t *nexprs, char *err, size_t errcap) {
  *nexprs = 0;
  if (err && errcap) err[0] = 0;
  if (!text) return 0;

  char perr[192] = {0};
  kdl_node_t *root = kdl_parse(text, perr, sizeof perr);
  if (!root) {
    if (err && errcap) snprintf(err, errcap, "%s", perr[0] ? perr : "bad chain");
    return 0;
  }

  size_t n = 0;
  for (size_t i = 0; i < root->nkids && n < max; i++) {
    const kdl_node_t *k = root->kids[i];
    if (!k || !k->name) continue;
    bool on_chrome = false;
    shader_t made;
    expr_prog_t *aexpr = NULL;
    char why[160] = {0};
    bool ok = parse_shader_entry(k, default_color, chrome, &on_chrome, &made,
                                 &aexpr, why, sizeof why);
    if (why[0] && err && errcap && !err[0]) snprintf(err, errcap, "%s", why);
    if (!ok) continue;
    /* One chain, one rect. The caller's rect is the *default*, so a line lifted
     * out of a config still means what it meant there, and an explicit `where=`
     * naming the other one is refused rather than quietly moved. */
    if (on_chrome != chrome) {
      if (err && errcap && !err[0])
        snprintf(err, errcap, "%s: this chain is %s", k->name,
                 chrome ? "chrome" : "content");
      expr_free(aexpr);
      continue;
    }
    out[n++] = made;
    if (aexpr) exprs[(*nexprs)++] = aexpr;
  }
  kdl_free(root);
  return n;
}

/* `direct` is part of the identity, not a property of it: `x` after the leader
 * and `x` on its own are two different bindings, and binding one must not
 * silently redefine the other. */
/* Every colour the theme knows, once. The parser walks it and so does the
 * renderer, so a colour cannot exist in one and not the other -- which is
 * exactly how config.kdl drifted from the code before this. */
static const struct {
  const char *name;
  size_t off;
} THEME_COLORS[] = {
    {"default_fg", offsetof(config_t, default_fg)},
    {"default_bg", offsetof(config_t, default_bg)},
    {"frame_focus", offsetof(config_t, frame_focus)},
    {"frame_idle", offsetof(config_t, frame_idle)},
    {"title", offsetof(config_t, title_focus)},
    {"title_idle", offsetof(config_t, title_idle)},
    {"button_fg", offsetof(config_t, button_fg)},
    {"button_bg", offsetof(config_t, button_bg)},
    {"button_bg_idle", offsetof(config_t, button_bg_idle)},
    {"guide", offsetof(config_t, guide)},
    {"resize", offsetof(config_t, resize)},
    {"drop_target", offsetof(config_t, drop_target)},
    {"scroll_fg", offsetof(config_t, scroll_fg)},
    {"scroll_bg", offsetof(config_t, scroll_bg)},
    {"header", offsetof(config_t, header)},
    {"header_hover", offsetof(config_t, header_hover)},
    {"header_hover_title", offsetof(config_t, header_hover_title)},
    {"tab_active_fg", offsetof(config_t, tab_active_fg)},
    {"tab_active_bg", offsetof(config_t, tab_active_bg)},
    {"tab_active_hover_fg", offsetof(config_t, tab_active_hover_fg)},
    {"tab_idle", offsetof(config_t, tab_idle)},
    {"tab_hover", offsetof(config_t, tab_hover)},
    {"prefix_fg", offsetof(config_t, prefix_fg)},
    {"prefix_bg", offsetof(config_t, prefix_bg)},
    {"tab_count", offsetof(config_t, tab_count)},
    {"status", offsetof(config_t, status)},
    {"status_state", offsetof(config_t, status_state)},
    {"finder_fg", offsetof(config_t, finder_fg)},
    {"finder_bg", offsetof(config_t, finder_bg)},
    {"finder_sel_fg", offsetof(config_t, finder_sel_fg)},
    {"finder_sel_bg", offsetof(config_t, finder_sel_bg)},
    {"bell", offsetof(config_t, bell)},
    {"modal_fg", offsetof(config_t, modal_fg)},
    {"modal_bg", offsetof(config_t, modal_bg)},
    {"modal_border", offsetof(config_t, modal_border)},
    {"modal_title", offsetof(config_t, modal_title)},
    {"modal_button", offsetof(config_t, modal_button)},
    {"modal_button_hover", offsetof(config_t, modal_button_hover)},
    {"dead", offsetof(config_t, dead)},
    {"hint", offsetof(config_t, hint)},
    {"minbar", offsetof(config_t, minbar)},
    {"minbar_hover", offsetof(config_t, minbar_hover)},
    {"pane_button", offsetof(config_t, pane_button)},
    {"pane_button_hover", offsetof(config_t, pane_button_hover)},
    {"rename_fg", offsetof(config_t, rename_fg)},
    {"rename_bg", offsetof(config_t, rename_bg)},
    {"toast_fg", offsetof(config_t, toast_fg)},
    {"toast_bg", offsetof(config_t, toast_bg)},
};

#define THEME_COLOR(c, i) \
  ((color_t *)((char *)(c) + THEME_COLORS[i].off))

static void bind_add(config_t *c, int key, uint16_t mods, action_t action,
                     bool direct) {
  for (size_t i = 0; i < c->nbinds; i++)
    if (c->binds[i].key == key && c->binds[i].mods == mods &&
        c->binds[i].direct == direct) {
      c->binds[i].action = action; /* a later binding replaces an earlier one */
      return;
    }
  c->binds = realloc(c->binds, (c->nbinds + 1) * sizeof *c->binds);
  c->binds[c->nbinds++] = (binding_t){key, mods, action, direct};
}

/* The knob and the states table are one mechanism: the knob writes the table.
 * `declared` is whether the config named `unfocused` itself, in which case it
 * has said what it wants -- including `states { unfocused { } }`, which says
 * "nothing", and must not be quietly refilled. */
/* Copy a mark, cutting only where a codepoint ends.
 *
 * snprintf truncates at a byte, and a mark is a grapheme cluster: cutting one
 * mid-codepoint puts bytes on the wire that are not UTF-8 at all, which shows
 * up far from here as a garbled screen rather than a too-short mark. */
static void set_mark(char *dst, size_t cap, const char *src) {
  size_t n = strlen(src);
  if (n >= cap) {
    n = cap - 1;
    while (n && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

static void apply_dim_unfocused(config_t *c, bool declared) {
  if (declared) return;
  if (!c->dim_unfocused) {
    c->state_n[PSTATE_UNFOCUSED] = 0;
    return;
  }
  shader_make(&c->state_shaders[PSTATE_UNFOCUSED][0], "dim", (color_t){0},
              c->dim_unfocused);
  c->state_n[PSTATE_UNFOCUSED] = 1;
}

void config_defaults(config_t *c) {
  memset(c, 0, sizeof *c);
  c->gap = 1;
  c->gap_aspect = 2;
  c->pad_top = c->pad_right = c->pad_bottom = c->pad_left = 0;
  c->rounded = true;
  c->title_align = ALIGN_CENTER;
  c->title_inset = 2;
  c->hints = true;
  c->version_banner = true;
  c->pane_buttons = true;
  /* Geometric shapes and a dingbat, because those are the blocks a font draws
   * at full cell size. The boxed operators these replaced (⊞ ⊟ ⊡ ⊠) were one
   * tidy family and unreadable with it: they are *math* symbols, sized to sit
   * beside x and y in an equation, so at a terminal's cell size the interior
   * that carried the whole meaning was two or three pixels of it.
   *
   * Legibility beats uniformity here, so these are deliberately different
   * shapes: a bar for away, an outline for "make it fill the tab", the same
   * outline filled in for "it is filling it", a cross for gone. The evenness
   * problem that put ASCII here originally is answered by picking glyphs that
   * fill their cell rather than by picking one family -- a glyph that does not
   * hands the slack to the gap beside it, by an amount the font decides.
   *
   * A *bar* for minimise rather than the obvious ▼, because the split guide
   * already owns all four triangles (◄ ► ▲ ▼) and the scroll indicator owns ▲
   * again: a glyph that means "split downward" on the border of a frame cannot
   * also mean "put this pane away" in its corner. Found by the guide's own test
   * failing, which is what that test is for.
   *
   * Single width, and not emoji: many terminals draw those two columns wide
   * while chrome here is booked as one. */
  snprintf(c->zoom_mark, sizeof c->zoom_mark, "\u25a1");       /* □ fill the tab */
  snprintf(c->zoom_on_mark, sizeof c->zoom_on_mark, "\u25a0"); /* ■ ...and back */
  snprintf(c->close_mark, sizeof c->close_mark, "\u2715");     /* ✕ close */
  snprintf(c->min_mark, sizeof c->min_mark, "\u25ac");         /* ▬ into the strip */
  snprintf(c->newtab_mark, sizeof c->newtab_mark, "+");
  c->bell_indicator = true;
  snprintf(c->bell_mark, sizeof c->bell_mark, "\u2022");
  c->keep_dead = KEEP_DEAD_COMMANDS;
  /* Gentle: an unfocused pane is one you are still reading half the time.
   * At 60 white text lands on #c3c3c3, which reads as "not this one" without
   * reading as "not available". */
  c->dim_unfocused = 60;
  c->min_pane_cols = 24;
  c->min_pane_rows = 6;
  c->min_split_cols = 32;
  c->min_split_rows = 8;
  c->scroll_lines = 3;
  c->toast_ms = 2500;
  c->hover_delay_ms = 250;
  c->double_click_ms = 400;
  /* 20fps. Fast enough that a pulse or a sweep reads as movement, slow enough
   * that an idle session with an animated shader is not a busy loop -- and it
   * only costs anything at all while such a shader is actually on screen. */
  c->anim_ms = 50;
  c->modal_scrim = 120;
  c->status_bar = true;
  c->status_line = true;
  /* Deliberately wider than the panes' own margin (gap * gap_aspect = 2), so
   * the strip and the line read as chrome sitting outside the layout rather
   * than as another row of it. */
  c->status_pad = 4;
  c->focus_follows_mouse = true;
  c->in_band_shaders = false;

  c->default_fg = rgb(0xff, 0xff, 0xff);
  c->default_bg = rgb(0x00, 0x00, 0x00);

  /* The one state with an opinion out of the box: while a pane is being
   * dragged, everything it could be dropped onto is pushed back so the pane in
   * your hand stands out. Everything else is empty until asked for. */
  shader_make(&c->state_shaders[PSTATE_DROP_TARGET][0], "grayscale",
              (color_t){0}, 200);
  shader_make(&c->state_shaders[PSTATE_DROP_TARGET][1], "dim", (color_t){0},
              140);
  c->state_n[PSTATE_DROP_TARGET] = 2;
  shader_make(&c->state_shaders[PSTATE_DROP_HOVER][0], "grayscale",
              (color_t){0}, 200);
  shader_make(&c->state_shaders[PSTATE_DROP_HOVER][1], "dim", (color_t){0},
              140);
  c->state_n[PSTATE_DROP_HOVER] = 2;
  /* The states with an opinion, and the line between those and the ones
   * without it:
   *
   *   a pane that is *not live* gets one — dead, suspended, scrolled. In all
   *   three the cells are showing something other than a running program's
   *   present: output from something that has exited, a pane that never
   *   started, or the past. You cannot discover any of that by looking unless
   *   something says so, and that is the whole argument for the feature;
   *
   *   a pane that is merely *not the one you are in* does not — unfocused,
   *   dragging. That is ambient contrast, which is a taste, and shipping a
   *   taste as a default is how a tool gets a reputation for fighting you.
   *
   * All three are gentle on purpose. A dead pane, a suspended one and
   * scrollback are all things you still want to read. */
  shader_make(&c->state_shaders[PSTATE_DEAD][0], "grayscale", (color_t){0},
              200);
  shader_make(&c->state_shaders[PSTATE_DEAD][1], "dim", (color_t){0}, 90);
  c->state_n[PSTATE_DEAD] = 2;

  /* Laid out and never started: inert, and still legible — a suspended pane's
   * contents are the command it is waiting to run. */
  shader_make(&c->state_shaders[PSTATE_SUSPENDED][0], "grayscale", (color_t){0},
              170);
  shader_make(&c->state_shaders[PSTATE_SUSPENDED][1], "dim", (color_t){0}, 60);
  c->state_n[PSTATE_SUSPENDED] = 2;

  apply_dim_unfocused(c, false);

  const color_t accent = rgb(0xff, 0x5f, 0xd7);
  const color_t ink = rgb(0x14, 0x14, 0x18);
  const color_t dim = rgb(0x45, 0x45, 0x4a);
  const color_t bright = rgb(0xff, 0xff, 0xff);

  c->frame_focus = accent;
  c->frame_idle = dim;
  c->title_focus = bright;
  c->title_idle = dim;

  c->button_fg = ink;
  c->button_bg = accent;
  c->button_bg_idle = rgb(0x55, 0x55, 0x5c);

  c->guide = accent;
  c->resize = accent;
  c->drop_target = accent;

  c->scroll_fg = ink;
  c->scroll_bg = accent;

  /* Looking at the past, said in colour as well as in the ▲ count: a wash of
   * the same accent the scroll indicator uses, so a theme moves both together
   * rather than leaving a hardcoded hue nobody can reach. Weak (about 9%) —
   * it has to survive being read through. Attached here rather than beside
   * the other states because it is the one that needs the palette. */
  shader_make(&c->state_shaders[PSTATE_SCROLLED][0], "tint", c->scroll_bg, 22);
  c->state_n[PSTATE_SCROLLED] = 1;

  c->header = dim;
  c->header_hover = accent;
  c->header_hover_title = bright;

  /* The tab you are in is stated in colour, not only in weight: bold alone is
   * easy to miss on a strip of short labels. */
  c->tab_active_fg = ink;
  c->tab_active_bg = accent;
  c->tab_active_hover_fg = bright;
  c->tab_idle = dim;
  c->tab_hover = accent;
  c->prefix_fg = ink;
  c->prefix_bg = accent;
  c->tab_count = dim;

  c->status = dim;
  c->status_state = bright;

  c->finder_fg = bright;
  c->finder_bg = dim;
  c->finder_sel_fg = ink;
  c->finder_sel_bg = accent;

  c->bell = accent;
  c->dead = rgb(0xff, 0x87, 0x5f);

  /* A surface, not a hole: lighter than the dimmed screen behind it, with a
   * border in the accent so the edge is never in doubt. The button is a grey
   * that is *visible* on that surface — the first version used the pane
   * button colour, which is the same value as the old background it was drawn
   * on, so it only appeared when you hovered it. */
  c->modal_bg = rgb(0x1c, 0x1c, 0x22);
  c->modal_fg = rgb(0xe8, 0xe8, 0xea);
  c->modal_border = accent;
  c->modal_title = bright;
  c->modal_button = rgb(0x8a, 0x8a, 0x95);
  c->modal_button_hover = accent;
  c->hint = bright;
  c->minbar = dim;
  c->minbar_hover = accent;
  c->pane_button = dim;
  c->pane_button_hover = accent;

  c->rename_fg = ink;
  c->rename_bg = accent;

  c->toast_fg = ink;
  c->toast_bg = accent;

  c->prefix_key = GHOSTTY_KEY_A;
  c->prefix_mods = MOD_CTRL;

  bind_add(c, GHOSTTY_KEY_BACKSLASH, 0, ACT_SPLIT_COLS, false);
  bind_add(c, GHOSTTY_KEY_MINUS, 0, ACT_SPLIT_ROWS, false);
  bind_add(c, GHOSTTY_KEY_X, 0, ACT_CLOSE_PANE, false);
  /* `?` twice, because whether it arrives with shift depends on the outer
   * terminal: as a plain byte there is no modifier to be had, and under the
   * kitty keyboard protocol (which the client asks for) there is. Binding one
   * of them is a binding that works on the author's machine. */
  bind_add(c, GHOSTTY_KEY_SLASH, MOD_SHIFT, ACT_HELP, false);
  bind_add(c, GHOSTTY_KEY_SLASH, 0, ACT_HELP, false);
  bind_add(c, GHOSTTY_KEY_R, 0, ACT_RERUN, false);
  bind_add(c, GHOSTTY_KEY_Z, 0, ACT_ZOOM, false);
  bind_add(c, GHOSTTY_KEY_E, 0, ACT_EDIT_CONFIG, false);
  bind_add(c, GHOSTTY_KEY_M, 0, ACT_MINIMIZE, false);
  bind_add(c, GHOSTTY_KEY_H, 0, ACT_FOCUS_LEFT, false);
  bind_add(c, GHOSTTY_KEY_L, 0, ACT_FOCUS_RIGHT, false);
  bind_add(c, GHOSTTY_KEY_K, 0, ACT_FOCUS_UP, false);
  bind_add(c, GHOSTTY_KEY_J, 0, ACT_FOCUS_DOWN, false);
  bind_add(c, GHOSTTY_KEY_ARROW_LEFT, 0, ACT_FOCUS_LEFT, false);
  bind_add(c, GHOSTTY_KEY_ARROW_RIGHT, 0, ACT_FOCUS_RIGHT, false);
  bind_add(c, GHOSTTY_KEY_ARROW_UP, 0, ACT_FOCUS_UP, false);
  bind_add(c, GHOSTTY_KEY_ARROW_DOWN, 0, ACT_FOCUS_DOWN, false);
  bind_add(c, GHOSTTY_KEY_O, 0, ACT_FOCUS_NEXT, false);
  bind_add(c, GHOSTTY_KEY_H, MOD_SHIFT, ACT_RESIZE_LEFT, false);
  bind_add(c, GHOSTTY_KEY_L, MOD_SHIFT, ACT_RESIZE_RIGHT, false);
  bind_add(c, GHOSTTY_KEY_K, MOD_SHIFT, ACT_RESIZE_UP, false);
  bind_add(c, GHOSTTY_KEY_J, MOD_SHIFT, ACT_RESIZE_DOWN, false);
  bind_add(c, GHOSTTY_KEY_ARROW_LEFT, MOD_SHIFT, ACT_RESIZE_LEFT, false);
  bind_add(c, GHOSTTY_KEY_ARROW_RIGHT, MOD_SHIFT, ACT_RESIZE_RIGHT, false);
  bind_add(c, GHOSTTY_KEY_ARROW_UP, MOD_SHIFT, ACT_RESIZE_UP, false);
  bind_add(c, GHOSTTY_KEY_ARROW_DOWN, MOD_SHIFT, ACT_RESIZE_DOWN, false);
  bind_add(c, GHOSTTY_KEY_EQUAL, 0, ACT_EQUALIZE, false);
  /* The leader and the space bar: the biggest key on the keyboard, no modifier,
   * and the one tmux already spends on cycling layouts — so the hand that knows
   * that reaches for the right thing here. Four presses come back round, which
   * is what makes a key this easy to hit the right choice rather than a hazard. */
  bind_add(c, GHOSTTY_KEY_SPACE, 0, ACT_ROTATE_LAYOUT, false);
  bind_add(c, GHOSTTY_KEY_C, 0, ACT_NEW_TAB, false);
  /* Cycling tabs is on tab/shift+tab, not on n/p: `p` is the palette, which is
   * pressed far more often than "the tab before this one" and had the only
   * shifted letter in the defaults. Tab is the key every other tabbed thing
   * cycles with, and it decodes on a plain terminal (`\e[Z` is shift+tab), so
   * this costs nothing on a client without the kitty protocol. */
  bind_add(c, GHOSTTY_KEY_TAB, 0, ACT_NEXT_TAB, false);
  bind_add(c, GHOSTTY_KEY_TAB, MOD_SHIFT, ACT_PREV_TAB, false);
  bind_add(c, GHOSTTY_KEY_F, 0, ACT_FINDER, false);
  bind_add(c, GHOSTTY_KEY_P, 0, ACT_PALETTE, false);
  bind_add(c, GHOSTTY_KEY_PAGE_UP, 0, ACT_SCROLL_PAGE_UP, false);
  bind_add(c, GHOSTTY_KEY_PAGE_DOWN, 0, ACT_SCROLL_PAGE_DOWN, false);
  bind_add(c, GHOSTTY_KEY_HOME, 0, ACT_SCROLL_TOP, false);
  bind_add(c, GHOSTTY_KEY_END, 0, ACT_SCROLL_BOTTOM, false);
  bind_add(c, GHOSTTY_KEY_D, 0, ACT_DETACH, false);
  bind_add(c, GHOSTTY_KEY_Q, 0, ACT_QUIT, false);
  for (int i = 0; i < 9; i++)
    bind_add(c, GHOSTTY_KEY_DIGIT_1 + i, 0,
             (action_t)(ACT_SELECT_TAB_1 + i), false);
}

void config_free(config_t *c) {
  for (size_t i = 0; i < c->nexprs; i++) expr_free(c->exprs[i]);
  free(c->exprs);
  free(c->binds);
  free(c->shell);
  free(c->editor);
  free(c->shader_dir);
  for (size_t i = 0; i < c->nfiles; i++) free(c->files[i]);
  memset(c, 0, sizeof *c);
}


/* ---- naming things, for the cheatsheet ---------------------------------- */

/* Phrases rather than the config's names: `split-cols` is what you write,
 * "split into columns" is what you are looking for when you have forgotten
 * which key does it. The order here is the order the list is drawn in. */
static const struct {
  action_t action;
  const char *group;
  const char *label;
} ACTION_HELP[] = {
    {ACT_SPLIT_COLS, "panes", "split into columns"},
    {ACT_SPLIT_ROWS, "panes", "split into rows"},
    {ACT_CLOSE_PANE, "panes", "close this pane"},
    {ACT_RERUN, "panes", "run a finished pane again"},
    {ACT_ZOOM, "panes", "fill the tab with it"},
    {ACT_MINIMIZE, "panes", "put it away in the strip"},

    {ACT_FOCUS_LEFT, "focus", "go left"},
    {ACT_FOCUS_RIGHT, "focus", "go right"},
    {ACT_FOCUS_UP, "focus", "go up"},
    {ACT_FOCUS_DOWN, "focus", "go down"},
    {ACT_FOCUS_NEXT, "focus", "the next pane"},
    {ACT_FINDER, "focus", "find a pane by name"},

    {ACT_RESIZE_LEFT, "size", "move the boundary left"},
    {ACT_RESIZE_RIGHT, "size", "move the boundary right"},
    {ACT_RESIZE_UP, "size", "move the boundary up"},
    {ACT_RESIZE_DOWN, "size", "move the boundary down"},
    /* Short enough to fit the palette's label column, which truncates at 26 —
     * "give every pane an even share" read as "give every pane an even sh". */
    {ACT_EQUALIZE, "size", "even out every split"},
    {ACT_ROTATE_LAYOUT, "size", "turn the layout a quarter"},

    {ACT_NEW_TAB, "tabs", "new tab"},
    {ACT_NEXT_TAB, "tabs", "next tab"},
    {ACT_PREV_TAB, "tabs", "previous tab"},
    {ACT_SELECT_TAB_1, "tabs", "go to that tab"},

    {ACT_SCROLL_UP, "scroll", "up a line"},
    {ACT_SCROLL_DOWN, "scroll", "down a line"},
    {ACT_SCROLL_PAGE_UP, "scroll", "up a page"},
    {ACT_SCROLL_PAGE_DOWN, "scroll", "down a page"},
    {ACT_SCROLL_TOP, "scroll", "to the oldest line"},
    {ACT_SCROLL_BOTTOM, "scroll", "back to the present"},

    {ACT_PALETTE, "session", "run a command"},
    {ACT_HELP, "session", "this list"},
    {ACT_EDIT_CONFIG, "session", "edit the config"},
    {ACT_DETACH, "session", "detach, leave it running"},
    {ACT_QUIT, "session", "quit the session"},
    {ACT_LITERAL_PREFIX, "session", "send the prefix itself"},
};

const char *config_action_name(action_t a) {
  for (size_t i = 0; i < sizeof ACTIONS / sizeof *ACTIONS; i++)
    if (ACTIONS[i].action == a) return ACTIONS[i].name;
  /* select-tab-N is nine actions from one row of the table. */
  static char buf[24];
  if (a >= ACT_SELECT_TAB_1 && a <= ACT_SELECT_TAB_1 + 8) {
    snprintf(buf, sizeof buf, "select-tab-%d", (int)(a - ACT_SELECT_TAB_1) + 1);
    return buf;
  }
  return NULL;
}

const char *config_action_label(action_t a) {
  for (size_t i = 0; i < sizeof ACTION_HELP / sizeof *ACTION_HELP; i++)
    if (ACTION_HELP[i].action == a) return ACTION_HELP[i].label;
  return NULL;
}

const char *config_action_group(action_t a) {
  for (size_t i = 0; i < sizeof ACTION_HELP / sizeof *ACTION_HELP; i++)
    if (ACTION_HELP[i].action == a) return ACTION_HELP[i].group;
  return NULL;
}

void config_chord_name(int key, uint16_t mods, char *out, size_t cap) {
  char base[24] = {0};

  /* Punctuation is written as the character you press, shifted or not: `?` is
   * a key on the keyboard and "S-slash" is a description of one. Both forms
   * are still valid in a config, so the sheet stays copyable. */
  static const struct {
    int key;
    const char *plain, *shifted;
  } PUNCT[] = {
      {GHOSTTY_KEY_SLASH, "/", "?"},      {GHOSTTY_KEY_BACKSLASH, "\\", "|"},
      {GHOSTTY_KEY_MINUS, "-", "_"},      {GHOSTTY_KEY_EQUAL, "=", "+"},
      {GHOSTTY_KEY_COMMA, ",", "<"},      {GHOSTTY_KEY_PERIOD, ".", ">"},
      {GHOSTTY_KEY_SEMICOLON, ";", ":"},  {GHOSTTY_KEY_QUOTE, "'", "\""},
      {GHOSTTY_KEY_BRACKET_LEFT, "[", "{"},
      {GHOSTTY_KEY_BRACKET_RIGHT, "]", "}"},
      {GHOSTTY_KEY_BACKQUOTE, "`", "~"},
  };
  /* Arrows as arrows. The cheatsheet is about the keyboard, and four words
   * where four glyphs will do costs a column of width on every row. */
  static const struct {
    int key;
    const char *glyph;
  } ARROWS[] = {
      {GHOSTTY_KEY_ARROW_LEFT, "\u2190"},  {GHOSTTY_KEY_ARROW_RIGHT, "\u2192"},
      {GHOSTTY_KEY_ARROW_UP, "\u2191"},    {GHOSTTY_KEY_ARROW_DOWN, "\u2193"},
  };

  for (size_t i = 0; i < sizeof PUNCT / sizeof *PUNCT; i++)
    if (PUNCT[i].key == key) {
      bool shifted = (mods & MOD_SHIFT) != 0;
      snprintf(base, sizeof base, "%s",
               shifted ? PUNCT[i].shifted : PUNCT[i].plain);
      if (shifted) mods &= (uint16_t)~MOD_SHIFT; /* spent on the glyph */
    }
  for (size_t i = 0; i < sizeof ARROWS / sizeof *ARROWS; i++)
    if (ARROWS[i].key == key) snprintf(base, sizeof base, "%s", ARROWS[i].glyph);

  if (!base[0])
    for (size_t i = 0; i < sizeof NAMED_KEYS / sizeof *NAMED_KEYS; i++)
      if (NAMED_KEYS[i].key == key)
        snprintf(base, sizeof base, "%s", NAMED_KEYS[i].name);

  if (!base[0] && key >= GHOSTTY_KEY_A && key <= GHOSTTY_KEY_Z) {
    char c = (char)('a' + (key - GHOSTTY_KEY_A));
    /* A shifted letter is written as the capital, because that is the key you
     * press. The modifier is spent here and not printed again below. */
    if (mods & MOD_SHIFT) {
      snprintf(base, sizeof base, "%c", (char)(c - 32));
      mods &= (uint16_t)~MOD_SHIFT;
    } else {
      snprintf(base, sizeof base, "%c", c);
    }
  }
  if (!base[0] && key >= GHOSTTY_KEY_DIGIT_0 && key <= GHOSTTY_KEY_DIGIT_9)
    snprintf(base, sizeof base, "%c", (char)('0' + (key - GHOSTTY_KEY_DIGIT_0)));
  if (!base[0]) snprintf(base, sizeof base, "?");

  snprintf(out, cap, "%s%s%s%s", mods & MOD_CTRL ? "C-" : "",
           mods & MOD_ALT ? "M-" : "", mods & MOD_SHIFT ? "S-" : "", base);
}


/* ---- rendering a config back out ----------------------------------------
 *
 * Every knob with the value it currently has, as a file you could have
 * written. Generated rather than kept as a copy on disk: a checked-in
 * "defaults" file is a second source of truth, and it drifts -- ours had
 * already lost four colours by the time anyone noticed.
 *
 * The comments here are one line each and say what a setting *is*. The long
 * form -- why a setting exists, what it cost to get right -- lives in
 * config/config.kdl, which is prose and belongs with the prose. Values are
 * generated; essays are written.
 */

typedef struct {
  char *buf;
  size_t len, cap;
} cfgbuf_t;

static void cb_add(cfgbuf_t *b, const char *fmt, ...) {
  va_list ap;
  for (;;) {
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < b->cap - b->len) {
      b->len += (size_t)n;
      return;
    }
    b->cap = b->cap ? b->cap * 2 : 4096;
    while (b->cap - b->len <= (size_t)n) b->cap *= 2;
    b->buf = realloc(b->buf, b->cap);
  }
}

static const char *yesno(bool v) { return v ? "true" : "false"; }

/* A chord as a KDL string: `\` and `"` are both keys somebody may have bound
 * and both end or escape a string, so they have to be written escaped. The
 * dump has to *parse back*, and a bare backslash there does not. */
/* A chord as a *config* writes it, which is not how the cheatsheet prints it.
 *
 * The dump used to use config_chord_name(), the display form -- so
 * `--dump-config`, documented as "a file you could have written", wrote
 * `prefix "C-a"` and `bind "S-←"` and eleven of its own lines came back as
 * complaints the next time it was read. The two notations overlap and are not
 * the same thing: the sheet is for a reader, this is for the parser.
 *
 * `shift+` is written out rather than folded into a capital or a shifted
 * punctuation mark: both are accepted on the way in, and one canonical spelling
 * on the way out means a dump of a dump is the same dump. */
static void cb_chord(cfgbuf_t *b, int key, uint16_t mods) {
  char chord[32] = {0};
  size_t n = 0;
  if (mods & MOD_CTRL) n += (size_t)snprintf(chord + n, sizeof chord - n, "ctrl+");
  if (mods & MOD_ALT) n += (size_t)snprintf(chord + n, sizeof chord - n, "alt+");
  if (mods & MOD_SHIFT) n += (size_t)snprintf(chord + n, sizeof chord - n, "shift+");
  if (mods & MOD_SUPER) n += (size_t)snprintf(chord + n, sizeof chord - n, "super+");

  const char *name = NULL;
  for (size_t i = 0; i < sizeof NAMED_KEYS / sizeof *NAMED_KEYS; i++)
    if (NAMED_KEYS[i].key == key) name = NAMED_KEYS[i].name;

  if (name) {
    snprintf(chord + n, sizeof chord - n, "%s", name);
  } else if (key >= GHOSTTY_KEY_A && key <= GHOSTTY_KEY_Z) {
    snprintf(chord + n, sizeof chord - n, "%c", (char)('a' + (key - GHOSTTY_KEY_A)));
  } else if (key >= GHOSTTY_KEY_DIGIT_0 && key <= GHOSTTY_KEY_DIGIT_9) {
    snprintf(chord + n, sizeof chord - n, "%c",
             (char)('0' + (key - GHOSTTY_KEY_DIGIT_0)));
  } else {
    /* The punctuation the parser knows, by the character you press. */
    static const struct { int key; char ch; } PUNCT[] = {
        {GHOSTTY_KEY_EQUAL, '='},        {GHOSTTY_KEY_BRACKET_LEFT, '['},
        {GHOSTTY_KEY_BRACKET_RIGHT, ']'}, {GHOSTTY_KEY_SEMICOLON, ';'},
        {GHOSTTY_KEY_QUOTE, '\''},       {GHOSTTY_KEY_BACKQUOTE, '`'},
    };
    char ch = 0;
    for (size_t i = 0; i < sizeof PUNCT / sizeof *PUNCT; i++)
      if (PUNCT[i].key == key) ch = PUNCT[i].ch;
    if (!ch) {
      /* A key with no spelling the parser would accept. Writing the display
       * form here is what caused this bug; writing nothing keeps the dump a
       * file that loads. */
      cb_add(b, "\"\"");
      return;
    }
    snprintf(chord + n, sizeof chord - n, "%c", ch);
  }

  cb_add(b, "\"");
  for (const char *q = chord; *q; q++) {
    if (*q == '"' || *q == '\\') cb_add(b, "\\%c", *q);
    else cb_add(b, "%c", *q);
  }
  cb_add(b, "\"");
}

static void cb_color(cfgbuf_t *b, const char *name, color_t c) {
  cb_add(b, "    %-22s \"#%02x%02x%02x\"\n", name, c.r, c.g, c.b);
}

static void cb_chain(cfgbuf_t *b, const char *indent, const shader_t *sh,
                     size_t n, bool chrome) {
  for (size_t i = 0; i < n; i++) {
    if (!sh[i].kind) continue;
    cb_add(b, "%s%s amount=%u", indent, sh[i].kind, sh[i].amount);
    if (sh[i].color.set)
      cb_add(b, " color=\"#%02x%02x%02x\"", sh[i].color.r, sh[i].color.g,
             sh[i].color.b);
    if (sh[i].param) cb_add(b, " at=%u", sh[i].param);
    if (chrome) cb_add(b, " where=\"chrome\"");
    if (sh[i].channels == SHADE_FG) cb_add(b, " channel=\"fg\"");
    else if (sh[i].channels == SHADE_BG) cb_add(b, " channel=\"bg\"");
    cb_add(b, "\n");
  }
}

char *config_render(const config_t *c) {
  cfgbuf_t b = {0};

  cb_add(&b, "// sl0ppty config, as it currently stands.\n");
  cb_add(&b, "//\n");
  cb_add(&b, "// Written by `sl0ppty --dump-config`, so every value here is\n");
  cb_add(&b, "// the one in force rather than one somebody typed up. Delete\n");
  cb_add(&b, "// anything you do not want to pin; what is missing is a\n");
  cb_add(&b, "// default, and defaults are allowed to improve.\n");
  cb_add(&b, "//\n");
  cb_add(&b, "// The commented reference -- what each setting is for, and why\n");
  cb_add(&b, "// it exists -- is config/config.kdl in the source tree.\n\n");

  cb_add(&b, "// ---- geometry ----\n");
  cb_add(&b, "gap %u\n", c->gap);
  cb_add(&b, "gap_aspect %u          // columns per row, so a gap looks square\n",
         c->gap_aspect);
  /* The shortest form that means what is in force, because `padding 0` reads
   * better than `padding 0 0 0 0` and round-trips to the same four numbers. */
  if (c->pad_top == c->pad_right && c->pad_top == c->pad_bottom &&
      c->pad_top == c->pad_left)
    cb_add(&b, "padding %u\n", c->pad_top);
  else if (c->pad_top == c->pad_bottom && c->pad_right == c->pad_left)
    cb_add(&b, "padding %u %u\n", c->pad_top, c->pad_right);
  else
    cb_add(&b, "padding %u %u %u %u   // top right bottom left\n", c->pad_top,
           c->pad_right, c->pad_bottom, c->pad_left);
  cb_add(&b, "rounded %s\n", yesno(c->rounded));
  cb_add(&b, "title_align \"%s\"\n",
         c->title_align == ALIGN_LEFT ? "left"
             : c->title_align == ALIGN_RIGHT ? "right" : "center");
  cb_add(&b, "title_inset %u\n", c->title_inset);
  cb_add(&b, "min_pane cols=%u rows=%u   // below this a pane collapses\n",
         c->min_pane_cols, c->min_pane_rows);
  cb_add(&b, "min_split cols=%u rows=%u  // below this a split is not offered\n",
         c->min_split_cols, c->min_split_rows);

  cb_add(&b, "\n// ---- what is on screen ----\n");
  cb_add(&b, "status_bar %s          // the strip along the top\n",
         yesno(c->status_bar));
  cb_add(&b, "status_line %s         // the line along the bottom\n",
         yesno(c->status_line));
  cb_add(&b, "status_pad %u\n", c->status_pad);
  cb_add(&b, "hints %s               // what the pointer is on, in the middle\n",
         yesno(c->hints));
  cb_add(&b, "version_banner %s      // ...and which build this is, when idle\n",
         yesno(c->version_banner));
  cb_add(&b, "pane_buttons %s        // the marks in a frame's top-right\n",
         yesno(c->pane_buttons));
  cb_add(&b, "bell_indicator %s\n", yesno(c->bell_indicator));
  cb_add(&b, "zoom_mark \"%s\"\n", c->zoom_mark);
  cb_add(&b, "zoom_on_mark \"%s\"\n", c->zoom_on_mark);
  cb_add(&b, "close_mark \"%s\"\n", c->close_mark);
  cb_add(&b, "min_mark \"%s\"\n", c->min_mark);
  cb_add(&b, "newtab_mark \"%s\"\n", c->newtab_mark);
  cb_add(&b, "bell_mark \"%s\"\n", c->bell_mark);

  cb_add(&b, "\n// ---- behaviour ----\n");
  cb_add(&b, "focus_follows_mouse %s\n", yesno(c->focus_follows_mouse));
  cb_add(&b, "in_band_shaders %s\n", yesno(c->in_band_shaders));
  cb_add(&b, "scroll_lines %u\n", c->scroll_lines);
  cb_add(&b, "toast_ms %u\n", c->toast_ms);
  cb_add(&b, "hover_delay_ms %u\n", c->hover_delay_ms);
  cb_add(&b, "double_click_ms %u\n", c->double_click_ms);
  cb_add(&b, "anim_ms %u             // frame clock while a shader animates\n",
         c->anim_ms);
  cb_add(&b, "modal_scrim %u         // how far a modal pushes the rest back\n",
         c->modal_scrim);
  cb_add(&b, "dim_unfocused %u       // ...and how far the panes you are not in\n",
         c->dim_unfocused);
  cb_add(&b, "keep_dead \"%s\"  // which dead panes stay: commands, all, none\n",
         c->keep_dead == KEEP_DEAD_ALL ? "all"
             : c->keep_dead == KEEP_DEAD_NONE ? "none" : "commands");
  if (c->shell) cb_add(&b, "shell \"%s\"\n", c->shell);
  else cb_add(&b, "// shell \"/bin/zsh\"     // unset: $SHELL\n");
  if (c->editor) cb_add(&b, "editor \"%s\"\n", c->editor);
  else cb_add(&b, "// editor \"nvim\"        // unset: $EDITOR, then vi\n");
  if (c->shader_dir) cb_add(&b, "shader_dir \"%s\"\n", c->shader_dir);
  else cb_add(&b, "// shader_dir \"~/.config/sl0ppty/shaders\"\n");

  cb_add(&b, "\n// ---- colour ----\ntheme {\n");
  for (size_t i = 0; i < sizeof THEME_COLORS / sizeof *THEME_COLORS; i++)
    cb_color(&b, THEME_COLORS[i].name, *THEME_COLOR((config_t *)c, i));
  cb_add(&b, "}\n");

  /* Written even when empty: an empty block says "this exists and you have
   * none", where nothing at all says "we forgot to tell you". Chrome passes
   * live in the same blocks, marked, because that is how they are written. */
  cb_add(&b, "\n// ---- colour passes over every pane ----\n");
  cb_add(&b, "// (contrib/shaders has thirty-odd to paste; contrib/shadertoy.html\n");
  cb_add(&b, "//  previews them. where=\"chrome\" runs a pass over the frame\n");
  cb_add(&b, "//  instead of the contents)\nshaders {\n");
  cb_chain(&b, "    ", c->shaders, c->nshaders, false);
  cb_chain(&b, "    ", c->chrome_shaders, c->nchrome_shaders, true);
  cb_add(&b, "}\n");

  cb_add(&b, "\n// ---- what a pane looks like in a given state ----\nstates {\n");
  for (int st = 0; st < PSTATE_COUNT; st++) {
    cb_add(&b, "    %s {\n", pane_state_name((pane_state_t)st));
    cb_chain(&b, "        ", c->state_shaders[st], c->state_n[st], false);
    cb_chain(&b, "        ", c->chrome_state_shaders[st],
             c->chrome_state_n[st], true);
    cb_add(&b, "    }\n");
  }
  cb_add(&b, "}\n");

  cb_add(&b, "\n// ---- keys ----\nkeys {\n");
  cb_add(&b, "    prefix ");
  cb_chord(&b, c->prefix_key, c->prefix_mods);
  cb_add(&b, "\n\n");
  for (size_t i = 0; i < c->nbinds; i++) {
    if (c->binds[i].direct) continue;
    const char *act = config_action_name(c->binds[i].action);
    if (!act) continue;
    cb_add(&b, "    bind ");
    cb_chord(&b, c->binds[i].key, c->binds[i].mods);
    cb_add(&b, " \"%s\"\n", act);
  }
  bool any_direct = false;
  for (size_t i = 0; i < c->nbinds; i++)
    if (c->binds[i].direct && config_action_name(c->binds[i].action))
      any_direct = true;
  if (any_direct) {
    cb_add(&b, "\n    // these fire with no leader, and are gone from every\n");
    cb_add(&b, "    // program in every pane\n    direct {\n");
    for (size_t i = 0; i < c->nbinds; i++) {
      if (!c->binds[i].direct) continue;
      const char *act = config_action_name(c->binds[i].action);
      if (!act) continue;
      cb_add(&b, "        bind ");
      cb_chord(&b, c->binds[i].key, c->binds[i].mods);
      cb_add(&b, " \"%s\"\n", act);
    }
    cb_add(&b, "    }\n");
  }
  cb_add(&b, "}\n");

  return b.buf;
}

char *config_dump_defaults(void) {
  config_t fresh;
  config_defaults(&fresh);
  char *text = config_render(&fresh);
  config_free(&fresh);
  return text;
}

action_t config_lookup(const config_t *c, int key, uint16_t mods) {
  /* caps/num lock must not make a binding stop working */
  mods &= (uint16_t)(MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_SUPER);
  /* Prefixed bindings first, then direct ones: after the leader, everything
   * that is bound works, so a chord that fires on its own does not stop
   * firing because you happened to press the leader before it. */
  for (size_t i = 0; i < c->nbinds; i++)
    if (!c->binds[i].direct && c->binds[i].key == key &&
        c->binds[i].mods == mods)
      return c->binds[i].action;
  return config_lookup_direct(c, key, mods);
}

action_t config_lookup_direct(const config_t *c, int key, uint16_t mods) {
  mods &= (uint16_t)(MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_SUPER);
  for (size_t i = 0; i < c->nbinds; i++)
    if (c->binds[i].direct && c->binds[i].key == key &&
        c->binds[i].mods == mods)
      return c->binds[i].action;
  return ACT_NONE;
}

bool config_has_direct(const config_t *c) {
  for (size_t i = 0; i < c->nbinds; i++)
    if (c->binds[i].direct && c->binds[i].action != ACT_NONE) return true;
  return false;
}

static action_t action_by_name(const char *name) {
  for (size_t i = 0; i < sizeof ACTIONS / sizeof *ACTIONS; i++)
    if (strcmp(ACTIONS[i].name, name) == 0) return ACTIONS[i].action;
  if (strncmp(name, "select-tab-", 11) == 0) {
    long n = strtol(name + 11, NULL, 10);
    if (n >= 1 && n <= 9) return (action_t)(ACT_SELECT_TAB_1 + (n - 1));
  }
  return ACT_NONE;
}

const char *config_default_path(void) {
  static char path[512];
  const char *explicit_ = getenv("SL0PPTY_CONFIG");
  if (explicit_ && *explicit_) {
    snprintf(path, sizeof path, "%s", explicit_);
    return path;
  }
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) snprintf(path, sizeof path, "%s/sl0ppty/config.kdl", xdg);
  else {
    const char *home = getenv("HOME");
    snprintf(path, sizeof path, "%s/.config/sl0ppty/config.kdl",
             home ? home : ".");
  }
  return path;
}

/* How deep a chain of includes may go. A depth limit rather than a set of files
 * already seen, because the two failures it has to stop are the same failure:
 * a config that includes itself and a config eleven files deep are both a
 * mistake, and the message says which one you have. */
#define INCLUDE_MAX_DEPTH 8

static bool load_into(config_t *c, const char *path, int depth, char *err,
                      size_t errcap);

/* Where `include "themes/nord.kdl"` points, given the file doing the
 * including. Relative to *that file*, not to the working directory: a config is
 * a thing on disk that refers to its neighbours, and where you happened to be
 * standing when you started the session is not part of what it means. */
static void include_path(const char *base_file, const char *ref, char *out,
                         size_t cap) {
  char buf[512];
  const char *r = path_expand(ref, buf, sizeof buf);
  if (r[0] == '/') {
    snprintf(out, cap, "%s", r);
    return;
  }
  char dir[512];
  snprintf(dir, sizeof dir, "%s", base_file);
  char *slash = strrchr(dir, '/');
  if (slash) *slash = 0;
  else snprintf(dir, sizeof dir, ".");
  snprintf(out, cap, "%s/%s", dir, r);
}

/* Every `include` at the top level of this file, in order, applied *before* the
 * file's own settings — wherever the line happens to sit. The loader reads a
 * document by asking it for the keys it knows rather than walking it in order,
 * so "here" is not a position it could honour; and the useful rule is the
 * simple one anyway: what you include is the base, what you write beside the
 * include wins. Later includes win over earlier ones, which is the one bit of
 * order that survives.
 *
 * A file that will not load is a line and no more (D9): the rest of the config
 * still applies, because losing your keybindings over a mistyped theme name
 * would be a worse answer than a session that says so.
 *
 * The complaint travels up whether or not the include itself loaded. A file that
 * applied while *its* include did not still has something to say, and the first
 * version only looked at the return value — so a bad file two levels down was
 * reported by nobody, because the file in the middle had parsed fine. */
static void apply_includes(config_t *c, const kdl_node_t *root, const char *path,
                           int depth, char *err, size_t errcap) {
  for (size_t i = 0; i < root->nkids; i++) {
    const kdl_node_t *n = root->kids[i];
    if (!n || !n->name || strcmp(n->name, "include") != 0) continue;
    if (!n->nargs) {
      complain(c, err, errcap, n->line, "include needs a file");
      continue;
    }
    for (size_t j = 0; j < n->nargs; j++) {
      char resolved[512];
      include_path(path, n->args[j], resolved, sizeof resolved);
      /* Its own buffer: an include's failure must not overwrite a message the
       * file that included it has already produced. */
      char ierr[256] = {0};
      if (!load_into(c, resolved, depth + 1, ierr, sizeof ierr) && !ierr[0])
        snprintf(ierr, sizeof ierr, "cannot include it");
      /* Already carries its own file and line: it was produced by the file
       * that has the problem, not by this one. */
      if (ierr[0] && err && errcap && !err[0]) snprintf(err, errcap, "%s", ierr);
    }
  }
}

bool config_load(config_t *c, const char *path, char *err, size_t errcap) {
  if (err && errcap) err[0] = 0;
  c->nmsgs = 0;
  bool ok = load_into(c, path, 0, err, errcap);
  c->loading = NULL; /* loader scratch: nothing may read it afterwards */
  return ok;
}

/* Remembered before the parse, not after: a file that is not there yet is
 * exactly the file worth watching, since writing it is the next thing that will
 * happen. Deduplicated, so a cycle does not fill the list with two names. */
static void remember_file(config_t *c, const char *path) {
  for (size_t i = 0; i < c->nfiles; i++)
    if (strcmp(c->files[i], path) == 0) return;
  if (c->nfiles >= CONFIG_FILES_MAX) return;
  char *dup = strdup(path);
  if (dup) c->files[c->nfiles++] = dup;
}

size_t config_files(const config_t *c, const char **out, size_t max) {
  size_t n = c->nfiles < max ? c->nfiles : max;
  for (size_t i = 0; i < n; i++) out[i] = c->files[i];
  return n;
}

/* The name to put in front of a complaint: the file it happened in, without its
 * directory. Enough to know which file to open when several are involved, and
 * short enough to fit in a toast. */
static const char *file_label(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool load_into(config_t *c, const char *path, int depth, char *err,
                      size_t errcap) {
  if (depth > INCLUDE_MAX_DEPTH) {
    complain(c, err, errcap, 0, "%s: includes nested too deep (a cycle?)",
             file_label(path));
    return false;
  }
  remember_file(c, path);
  char kerr[192] = {0};
  kdl_node_t *root = kdl_parse_file(path, kerr, sizeof kerr);
  if (!root) {
    /* Its own message: kdl reports the path itself, so this one is not given a
     * file prefix. Defaults stand; the caller reports why. */
    const char *was = c->loading;
    c->loading = NULL;
    complain(c, err, errcap, 0, "%s", kerr[0] ? kerr : "cannot read it");
    c->loading = was;
    return false;
  }

  apply_includes(c, root, path, depth, err, errcap);
  /* After the includes, because each of those set it to its own file while it
   * was being read. From here the complaints belong to this file. */
  c->loading = path;

  c->gap = (uint16_t)kdl_arg_int(kdl_child(root, "gap"), 0, c->gap);
  c->gap_aspect =
      (uint16_t)kdl_arg_int(kdl_child(root, "gap_aspect"), 0, c->gap_aspect);
  /* `padding 1`, `padding 0 2`, or `padding 1 2 1 2` -- one value for every
   * side, two for vertical and horizontal, four in CSS order (top, right,
   * bottom, left), because that is the order everybody who has written a
   * stylesheet already knows. Three is refused rather than guessed at: CSS says
   * top/horizontal/bottom and a reader who has to look that up is a reader who
   * cannot see what the line does. */
  {
    const kdl_node_t *pn = kdl_child(root, "padding");
    if (pn) {
      long v[4];
      size_t n = pn->nargs < 4 ? pn->nargs : 4;
      for (size_t i = 0; i < n; i++) v[i] = kdl_arg_int(pn, i, 0);
      if (pn->nargs == 1) {
        c->pad_top = c->pad_right = c->pad_bottom = c->pad_left = (uint16_t)v[0];
      } else if (pn->nargs == 2) {
        c->pad_top = c->pad_bottom = (uint16_t)v[0];
        c->pad_right = c->pad_left = (uint16_t)v[1];
      } else if (pn->nargs == 4) {
        c->pad_top = (uint16_t)v[0];
        c->pad_right = (uint16_t)v[1];
        c->pad_bottom = (uint16_t)v[2];
        c->pad_left = (uint16_t)v[3];
      } else {
        complain(c, err, errcap, pn->line,
                 "padding takes 1, 2 or 4 values (all, vertical horizontal, or "
                 "top right bottom left), not %zu", pn->nargs);
      }
    }
  }
  c->rounded = kdl_arg_bool(kdl_child(root, "rounded"), 0, c->rounded);
  c->status_bar = kdl_arg_bool(kdl_child(root, "status_bar"), 0, c->status_bar);
  c->status_line =
      kdl_arg_bool(kdl_child(root, "status_line"), 0, c->status_line);
  c->status_pad =
      (uint16_t)kdl_arg_int(kdl_child(root, "status_pad"), 0, c->status_pad);
  c->focus_follows_mouse = kdl_arg_bool(kdl_child(root, "focus_follows_mouse"), 0,
                                        c->focus_follows_mouse);
  c->in_band_shaders = kdl_arg_bool(kdl_child(root, "in_band_shaders"), 0,
                                    c->in_band_shaders);

  const char *align = kdl_arg(kdl_child(root, "title_align"), 0, NULL);
  if (align) {
    if (strcmp(align, "left") == 0) c->title_align = ALIGN_LEFT;
    else if (strcmp(align, "right") == 0) c->title_align = ALIGN_RIGHT;
    else c->title_align = ALIGN_CENTER;
  }

  c->title_inset =
      (uint16_t)kdl_arg_int(kdl_child(root, "title_inset"), 0, c->title_inset);
  c->hints = kdl_arg_bool(kdl_child(root, "hints"), 0, c->hints);
  c->version_banner =
      kdl_arg_bool(kdl_child(root, "version_banner"), 0, c->version_banner);
  c->pane_buttons =
      kdl_arg_bool(kdl_child(root, "pane_buttons"), 0, c->pane_buttons);
  const char *zm = kdl_arg(kdl_child(root, "zoom_mark"), 0, NULL);
  if (zm) set_mark(c->zoom_mark, sizeof c->zoom_mark, zm);
  const char *zo = kdl_arg(kdl_child(root, "zoom_on_mark"), 0, NULL);
  if (zo) set_mark(c->zoom_on_mark, sizeof c->zoom_on_mark, zo);
  const char *cm = kdl_arg(kdl_child(root, "close_mark"), 0, NULL);
  if (cm) set_mark(c->close_mark, sizeof c->close_mark, cm);
  const char *mm = kdl_arg(kdl_child(root, "min_mark"), 0, NULL);
  if (mm) set_mark(c->min_mark, sizeof c->min_mark, mm);
  const char *nt = kdl_arg(kdl_child(root, "newtab_mark"), 0, NULL);
  if (nt) set_mark(c->newtab_mark, sizeof c->newtab_mark, nt);
  {
    /* `commands` (the default), `all`, or `none`. `true`/`false` are taken as
     * `all`/`none`, because that is what they used to mean here and a config
     * that still says so should keep working rather than silently changing
     * behaviour. */
    const char *kd = kdl_arg(kdl_child(root, "keep_dead"), 0, NULL);
    if (kd) {
      if (!strcmp(kd, "all") || !strcmp(kd, "true")) c->keep_dead = KEEP_DEAD_ALL;
      else if (!strcmp(kd, "none") || !strcmp(kd, "false"))
        c->keep_dead = KEEP_DEAD_NONE;
      else if (!strcmp(kd, "commands")) c->keep_dead = KEEP_DEAD_COMMANDS;
      else
        complain(c, err, errcap, kdl_child(root, "keep_dead")->line,
                 "keep_dead: %s (want commands, all or none)", kd);
    }
  }
  {
    long v = kdl_arg_int(kdl_child(root, "dim_unfocused"), 0, c->dim_unfocused);
    c->dim_unfocused = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
  }
  c->bell_indicator =
      kdl_arg_bool(kdl_child(root, "bell_indicator"), 0, c->bell_indicator);
  const char *bm = kdl_arg(kdl_child(root, "bell_mark"), 0, NULL);
  if (bm) set_mark(c->bell_mark, sizeof c->bell_mark, bm);

  const kdl_node_t *mins = kdl_child(root, "min_split");
  if (mins) {
    c->min_split_cols =
        (uint16_t)kdl_prop_int(mins, "cols", c->min_split_cols);
    c->min_split_rows =
        (uint16_t)kdl_prop_int(mins, "rows", c->min_split_rows);
  }

  const kdl_node_t *minp = kdl_child(root, "min_pane");
  if (minp) {
    c->min_pane_cols = (uint16_t)kdl_prop_int(minp, "cols", c->min_pane_cols);
    c->min_pane_rows = (uint16_t)kdl_prop_int(minp, "rows", c->min_pane_rows);
  }
  c->scroll_lines =
      (uint16_t)kdl_arg_int(kdl_child(root, "scroll_lines"), 0, c->scroll_lines);
  c->toast_ms = (uint16_t)kdl_arg_int(kdl_child(root, "toast_ms"), 0, c->toast_ms);
  c->hover_delay_ms = (uint16_t)kdl_arg_int(kdl_child(root, "hover_delay_ms"), 0,
                                            c->hover_delay_ms);
  c->double_click_ms = (uint16_t)kdl_arg_int(kdl_child(root, "double_click_ms"),
                                             0, c->double_click_ms);
  c->anim_ms =
      (uint16_t)kdl_arg_int(kdl_child(root, "anim_ms"), 0, c->anim_ms);
  {
    long v = kdl_arg_int(kdl_child(root, "modal_scrim"), 0, c->modal_scrim);
    c->modal_scrim = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
  }


  const char *sh = kdl_arg(kdl_child(root, "shell"), 0, NULL);
  if (sh) {
    free(c->shell);
    c->shell = strdup(sh);
  }
  const char *ed = kdl_arg(kdl_child(root, "editor"), 0, NULL);
  if (ed) {
    free(c->editor);
    c->editor = strdup(ed);
  }

  /* Shader plugins, before any shader is named below: a `shaders` block may
   * use what one of these adds, and a name is looked up as it is parsed. The
   * default is a directory beside this file, so dropping a `.so` next to the
   * config is the whole installation procedure. */
  const char *sdir = kdl_arg(kdl_child(root, "shader_dir"), 0, NULL);
  if (sdir) {
    free(c->shader_dir);
    c->shader_dir = strdup(sdir);
  }
  {
    char dir[1024];
    if (c->shader_dir) {
      char buf[1024];
      snprintf(dir, sizeof dir, "%s",
               path_expand(c->shader_dir, buf, sizeof buf));
    } else {
      snprintf(dir, sizeof dir, "%s", path);
      char *slash = strrchr(dir, '/');
      if (slash) slash[1] = 0;
      else dir[0] = 0;
      snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "shaders");
    }
    char lerr[256] = {0};
    shader_load_dir(dir, lerr, sizeof lerr);
    /* A plugin that will not load is worth a line, and worth nothing more:
     * the config it came with still works, minus that effect (D9). */
    if (lerr[0]) complain(c, err, errcap, 0, "%s", lerr);
  }

  /* One node per pass, in the order written, because a chain is a sequence.
   * Shared by `shaders { }` and by every state, so a state's chain is exactly
   * as expressive as the global one and there is one parser to be wrong. */
  const kdl_node_t *shaders = kdl_child(root, "shaders");
  if (shaders)
    parse_shader_list(c, shaders, c->shaders, &c->nshaders, c->chrome_shaders,
                      &c->nchrome_shaders, err, errcap);

  /* `states { drop_target { grayscale amount=200; dim amount=140 } }` — what a
   * pane looks like while it is in a state. Naming a state at all replaces its
   * default outright, including with nothing, which is how you turn one off. */
  const kdl_node_t *states = kdl_child(root, "states");
  bool unfocused_declared = false;
  if (states) {
    for (size_t i = 0; i < states->nkids; i++) {
      const kdl_node_t *k = states->kids[i];
      if (!k || !k->name) continue;
      pane_state_t st = PSTATE_COUNT;
      for (size_t j = 0; j < PSTATE_COUNT; j++)
        if (strcmp(pane_state_name((pane_state_t)j), k->name) == 0)
          st = (pane_state_t)j;
      if (st == PSTATE_COUNT) {
        complain(c, err, errcap, k->line, "unknown pane state: %s", k->name);
        continue;
      }
      if (st == PSTATE_UNFOCUSED) unfocused_declared = true;
      parse_shader_list(c, k, c->state_shaders[st], &c->state_n[st],
                        c->chrome_state_shaders[st], &c->chrome_state_n[st],
                        err, errcap);
    }
  }
  /* After the states block, so a config that wrote its own chain keeps it and
   * one that did not gets the knob's. */
  apply_dim_unfocused(c, unfocused_declared);

  const kdl_node_t *theme = kdl_child(root, "theme");
  if (theme) {
    for (size_t i = 0; i < sizeof THEME_COLORS / sizeof *THEME_COLORS; i++) {
      const char *v = kdl_arg(kdl_child(theme, THEME_COLORS[i].name), 0, NULL);
      if (!v) continue;
      if (!parse_color(v, THEME_COLOR(c, i)))
        complain(c, err, errcap, kdl_child(theme, THEME_COLORS[i].name)->line,
                 "bad colour for %s: %s", THEME_COLORS[i].name, v);
    }
  }

  const kdl_node_t *keys = kdl_child(root, "keys");
  if (keys) {
    const char *pfx = kdl_arg(kdl_child(keys, "prefix"), 0, NULL);
    if (pfx) {
      int k;
      uint16_t m;
      if (config_parse_chord(pfx, &k, &m)) {
        c->prefix_key = k;
        c->prefix_mods = m;
      } else {
        complain(c, err, errcap, kdl_child(keys, "prefix")->line,
                 "bad prefix: %s", pfx);
      }
    }
    /* `bind` under `keys` needs the leader; `bind` under `keys { direct { } }`
     * does not. A block rather than a property on each line, because these
     * take a chord away from every program in every pane and that is worth
     * being able to see at a glance — and worth having somewhere to write the
     * warning down. */
    for (size_t i = 0; i < keys->nkids; i++) {
      const kdl_node_t *node = keys->kids[i];
      bool direct = strcmp(node->name, "direct") == 0;
      if (!direct && strcmp(node->name, "bind") != 0) continue;

      size_t count = direct ? node->nkids : 1;
      for (size_t j = 0; j < count; j++) {
        const kdl_node_t *b = direct ? node->kids[j] : node;
        if (!b || strcmp(b->name, "bind") != 0) continue;
        const char *chord = kdl_arg(b, 0, NULL);
        const char *act = kdl_arg(b, 1, NULL);
        int k;
        uint16_t m;
        bool implied = false;
        if (!chord || !act || !parse_chord_ex(chord, &k, &m, &implied)) {
          /* Naming the chord, because "bad binding" on line 14 of a file you
           * did not write by hand is a hunt rather than a message. */
          complain(c, err, errcap, b->line, "bad key: %s",
                   chord ? chord : "(none)");
          continue;
        }
        action_t a = action_by_name(act);
        if (a == ACT_NONE && strcmp(act, "none") != 0) {
          complain(c, err, errcap, b->line, "unknown action: %s", act);
          continue;
        }
        bind_add(c, k, m, a, direct);
        /* ...and the same key without it, because whether `?` arrives as
         * shift+slash or as a bare slash depends on the terminal, not on what
         * the config meant. */
        if (implied) bind_add(c, k, (uint16_t)(m & ~MOD_SHIFT), a, direct);
      }
    }
  }

  kdl_free(root);

  return true;
}
