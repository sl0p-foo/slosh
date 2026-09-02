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
    {"split", ACT_SPLIT},
    {"split-cols", ACT_SPLIT_COLS},
    {"split-rows", ACT_SPLIT_ROWS},
    {"close-pane", ACT_CLOSE_PANE},
    {"rerun", ACT_RERUN},
    {"zoom", ACT_ZOOM},
    {"minimize", ACT_MINIMIZE},
    {"float", ACT_FLOAT},
    {"new-float", ACT_NEW_FLOAT},
    {"float-grow", ACT_FLOAT_GROW},
    {"float-shrink", ACT_FLOAT_SHRINK},
    {"set-purpose", ACT_SET_PURPOSE},
    {"rename-pane", ACT_RENAME_PANE},
    {"rename-tab", ACT_RENAME_TAB},
    {"focus-left", ACT_FOCUS_LEFT},
    {"focus-right", ACT_FOCUS_RIGHT},
    {"focus-up", ACT_FOCUS_UP},
    {"focus-down", ACT_FOCUS_DOWN},
    {"focus-next", ACT_FOCUS_NEXT},
    {"resize-left", ACT_RESIZE_LEFT},
    {"resize-right", ACT_RESIZE_RIGHT},
    {"resize-up", ACT_RESIZE_UP},
    {"resize-down", ACT_RESIZE_DOWN},
    {"equalize", ACT_EQUALIZE},
    {"clear-shaders", ACT_CLEAR_SHADERS},
    {"pane-to-next-tab", ACT_PANE_TO_NEXT_TAB},
    {"pane-to-prev-tab", ACT_PANE_TO_PREV_TAB},
    {"pane-to-new-tab", ACT_PANE_TO_NEW_TAB},
    {"rotate-layout", ACT_ROTATE_LAYOUT},
    {"scroll-up", ACT_SCROLL_UP},
    {"scroll-down", ACT_SCROLL_DOWN},
    {"scroll-page-up", ACT_SCROLL_PAGE_UP},
    {"scroll-page-down", ACT_SCROLL_PAGE_DOWN},
    {"scroll-top", ACT_SCROLL_TOP},
    {"scroll-bottom", ACT_SCROLL_BOTTOM},
    {"new-tab", ACT_NEW_TAB},
    {"next-tab", ACT_NEXT_TAB},
    {"close-tab", ACT_CLOSE_TAB},
    {"prev-tab", ACT_PREV_TAB},
    {"finder", ACT_FINDER},
    {"palette", ACT_PALETTE},
    {"workspaces", ACT_WORKSPACES},
    {"save-workspace", ACT_SAVE_WORKSPACE},
    {"detach", ACT_DETACH},
    {"quit", ACT_QUIT},
    {"literal-prefix", ACT_LITERAL_PREFIX},
    {"help", ACT_HELP},
    {"edit-config", ACT_EDIT_CONFIG},
};

static const struct {
  const char *name;
  int key;
} NAMED_KEYS[] = {
    {"left", GHOSTTY_KEY_ARROW_LEFT},
    {"right", GHOSTTY_KEY_ARROW_RIGHT},
    {"up", GHOSTTY_KEY_ARROW_UP},
    {"down", GHOSTTY_KEY_ARROW_DOWN},
    {"enter", GHOSTTY_KEY_ENTER},
    {"tab", GHOSTTY_KEY_TAB},
    {"escape", GHOSTTY_KEY_ESCAPE},
    {"space", GHOSTTY_KEY_SPACE},
    {"backspace", GHOSTTY_KEY_BACKSPACE},
    {"home", GHOSTTY_KEY_HOME},
    {"end", GHOSTTY_KEY_END},
    {"pageup", GHOSTTY_KEY_PAGE_UP},
    {"pagedown", GHOSTTY_KEY_PAGE_DOWN},
    {"delete", GHOSTTY_KEY_DELETE},
    {"insert", GHOSTTY_KEY_INSERT},
    {"backslash", GHOSTTY_KEY_BACKSLASH},
    {"minus", GHOSTTY_KEY_MINUS},
    {"slash", GHOSTTY_KEY_SLASH},
    {"comma", GHOSTTY_KEY_COMMA},
    {"period", GHOSTTY_KEY_PERIOD},
    /* The function row. The decoder has always understood these (legacy
     * CSI ~-forms, SS3 and kitty alike); the config simply had no name for
     * them, so nothing could be bound there. */
    {"f1", GHOSTTY_KEY_F1},
    {"f2", GHOSTTY_KEY_F2},
    {"f3", GHOSTTY_KEY_F3},
    {"f4", GHOSTTY_KEY_F4},
    {"f5", GHOSTTY_KEY_F5},
    {"f6", GHOSTTY_KEY_F6},
    {"f7", GHOSTTY_KEY_F7},
    {"f8", GHOSTTY_KEY_F8},
    {"f9", GHOSTTY_KEY_F9},
    {"f10", GHOSTTY_KEY_F10},
    {"f11", GHOSTTY_KEY_F11},
    {"f12", GHOSTTY_KEY_F12},
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
    {"\u2190", GHOSTTY_KEY_ARROW_LEFT},
    {"\u2192", GHOSTTY_KEY_ARROW_RIGHT},
    {"\u2191", GHOSTTY_KEY_ARROW_UP},
    {"\u2193", GHOSTTY_KEY_ARROW_DOWN},
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
    if (strncmp(p, "ctrl+", 5) == 0) {
      mods |= MOD_CTRL;
      p += 5;
    } else if (strncmp(p, "alt+", 4) == 0) {
      mods |= MOD_ALT;
      p += 4;
    } else if (strncmp(p, "shift+", 6) == 0) {
      mods |= MOD_SHIFT;
      p += 6;
    } else if (strncmp(p, "super+", 6) == 0) {
      mods |= MOD_SUPER;
      p += 6;
    }
    /* The cheatsheet's own shorthand. It prints `C-a`, `M-x`, `S-tab`, and the
     * whole claim of that sheet is that what it shows is what you would write
     * -- which was false for every chord it printed with a modifier. */
    else if (strncmp(p, "C-", 2) == 0) {
      mods |= MOD_CTRL;
      p += 2;
    } else if (strncmp(p, "M-", 2) == 0) {
      mods |= MOD_ALT;
      p += 2;
    } else if (strncmp(p, "S-", 2) == 0) {
      mods |= MOD_SHIFT;
      p += 2;
    } else
      break;
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
    snprintf(full, sizeof full, "%s:%d: %s", base ? base + 1 : file, line,
             text);
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
    "dragging", "drop_hover", "drop_target", "dead",      "suspended",
    "bell",     "scrolled",   "floating",    "unfocused",
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
                               shader_t *out, expr_prog_t **expr, char *why,
                               size_t whycap) {
  *expr = NULL;

  /* Every parameter a shader takes is a property, so a bare argument is not a
   * thing we ignore -- it is a `;` somebody left out. `tint color="#000000"
   * spotlight` reads to a person as two passes and to KDL as one node with a
   * spare word, and answering "ok" to that is how you spend ten minutes
   * wondering why the second one does nothing. */
  if (k->nargs) {
    snprintf(why, whycap,
             "unexpected argument for %s: %s (separate entries with `;`)",
             k->name, kdl_arg(k, 0, ""));
    return false;
  }

  /* Where this pass runs. Dropped rather than defaulted when the word is not
   * one we know, for the same reason a bad `amount` is: the entry says what it
   * wants and we cannot do it, and running it over the contents because "chrom"
   * was a typo would be a surprise nobody asked for. */
  const char *where =
      kdl_prop(k, "where", default_chrome ? "chrome" : "content");
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
  if (strcmp(chan, "fg") == 0)
    channels = SHADE_FG;
  else if (strcmp(chan, "bg") == 0)
    channels = SHADE_BG;
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
  if (bad_colour)
    snprintf(why, whycap, "bad colour for shader %s: %s", k->name, cs);
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

/* Entry nodes into two chains, routed by each one's `where=` -- the only place
 * that routing is decided, whether the entries came from a config file, from a
 * preset file, or from a line somebody typed at a prompt.
 *
 * `default_chrome` is what an entry that does not say `where=` means. A config
 * file's default is content; a prompt aimed at the frame passes true, so a line
 * lifted out of a config still means what it meant there and a line typed at
 * `chrome>` means what the prompt says. Either way an explicit `where=` wins: a
 * document says where its own passes go, and the caller's is a *default*.
 *
 * Expressions come back in `exprs` for the caller to free; entries past SHADE_MAX
 * for their rect are dropped, and the first refusal lands in `err`. */
static size_t route_entries(const kdl_node_t *const *kids, size_t nkids,
                            color_t default_color, bool default_chrome,
                            shader_t *content, size_t *ncontent,
                            shader_t *chrome, size_t *nchrome,
                            expr_prog_t **exprs, size_t *nexprs, char *err,
                            size_t errcap) {
  size_t total = 0;
  for (size_t i = 0; i < nkids; i++) {
    const kdl_node_t *k = kids[i];
    if (!k || !k->name) continue;

    bool on_chrome = false;
    shader_t made;
    expr_prog_t *aexpr = NULL;
    char why[160] = {0};
    bool ok = parse_shader_entry(k, default_color, default_chrome, &on_chrome,
                                 &made, &aexpr, why, sizeof why);
    if (why[0] && err && errcap && !err[0]) snprintf(err, errcap, "%s", why);
    if (!ok) continue;

    shader_t *out = on_chrome ? chrome : content;
    size_t *n = on_chrome ? nchrome : ncontent;
    if (*n >= SHADE_MAX) {
      if (err && errcap && !err[0])
        snprintf(err, errcap, "more than %d passes for the %s", SHADE_MAX,
                 on_chrome ? "frame" : "contents");
      expr_free(aexpr);
      continue;
    }
    out[(*n)++] = made;
    if (aexpr) exprs[(*nexprs)++] = aexpr;
    total++;
  }
  return total;
}

/* Chains from text: what a `shaders { }` block holds, as a person types it at a
 * prompt -- one entry, several separated by `;`, or the whole block with its
 * braces around it. Every shape a config file has, parsed by the config's own
 * parser, so what you prototype is what you can paste and the other way round. */
size_t config_parse_chain_doc(const char *text, color_t default_color,
                              bool default_chrome, shader_t *content,
                              size_t *ncontent, shader_t *chrome,
                              size_t *nchrome, expr_prog_t **exprs,
                              size_t *nexprs, char *err, size_t errcap) {
  *ncontent = 0;
  *nchrome = 0;
  *nexprs = 0;
  if (err && errcap) err[0] = 0;
  if (!text) return 0;

  char perr[192] = {0};
  kdl_node_t *root = kdl_parse(text, perr, sizeof perr);
  if (!root) {
    if (err && errcap)
      snprintf(err, errcap, "%s", perr[0] ? perr : "bad chain");
    return 0;
  }

  /* `shaders { ... }` around the entries, or the entries on their own. Both are
   * things a config file contains, so both are things this takes: the wrapper is
   * what `:paste` prints, and therefore what somebody will paste back. */
  const kdl_node_t *block = NULL;
  for (size_t i = 0; i < root->nkids; i++) {
    const kdl_node_t *k = root->kids[i];
    if (k && k->name && strcmp(k->name, "shaders") == 0) block = k;
  }
  const kdl_node_t *const *kids = block ? (const kdl_node_t *const *)block->kids
                                        : (const kdl_node_t *const *)root->kids;
  size_t nkids = block ? block->nkids : root->nkids;

  size_t n =
      route_entries(kids, nkids, default_color, default_chrome, content,
                    ncontent, chrome, nchrome, exprs, nexprs, err, errcap);
  kdl_free(root);
  return n;
}

/* A preset file into two chains: the same `shaders { }` block a config carries,
 * routed by each entry's `where=` exactly as the config routes it.
 *
 * Here rather than in the caller because the parsing is already here, and a second
 * reader of this format -- a scanner counting braces and skipping `//` outside
 * quoted strings -- would be a second answer to "what does this file say". The KDL
 * parser knows about comments and strings; nobody else needs to.
 *
 * Anything else at the top level is ignored, so a whole config.kdl can be handed
 * over and only its shaders apply: `theme` and `keys` are not this pane's business,
 * and refusing the file over them would make the useful case the awkward one. */
size_t config_parse_chain_file(const char *path, color_t default_color,
                               shader_t *content, size_t *ncontent,
                               shader_t *chrome, size_t *nchrome,
                               expr_prog_t **exprs, size_t *nexprs, char *err,
                               size_t errcap) {
  *ncontent = 0;
  *nchrome = 0;
  *nexprs = 0;
  if (err && errcap) err[0] = 0;

  char perr[192] = {0};
  kdl_node_t *root = kdl_parse_file(path, perr, sizeof perr);
  if (!root) {
    if (err && errcap)
      snprintf(err, errcap, "%s", perr[0] ? perr : "cannot read it");
    return 0;
  }

  size_t total = 0;
  bool found = false, states = false;
  for (size_t i = 0; i < root->nkids; i++) {
    const kdl_node_t *block = root->kids[i];
    if (!block || !block->name) continue;
    if (strcmp(block->name, "states") == 0) states = true;
    if (strcmp(block->name, "shaders") != 0) continue;
    found = true;
    /* A config file's default is content, and this is a config file. */
    total += route_entries((const kdl_node_t *const *)block->kids, block->nkids,
                           default_color, false, content, ncontent, chrome,
                           nchrome, exprs, nexprs, err, errcap);
  }
  kdl_free(root);
  if (!found && err && errcap && !err[0]) {
    /* Naming the likely mistake rather than the general one: a file of nothing but
     * `states { }` is a config's opinion about every pane, and a pane cannot wear
     * one -- an in-band chain has no state to hang off. */
    if (states)
      snprintf(err, errcap,
               "only states { } in it, which is a config's and not a pane's -- "
               "paste the pass itself");
    else
      snprintf(err, errcap, "no shaders { } block in it");
  }
  return total;
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
    {"attach_fg", offsetof(config_t, attach_fg)},
    {"attach_bg", offsetof(config_t, attach_bg)},
};

#define THEME_COLOR(c, i) ((color_t *)((char *)(c) + THEME_COLORS[i].off))

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

/* One derived pass: a shader whose amount is a compiled-in expression,
 * written into `sh` with its program registered on the config, which owns
 * every program. The sources are string literals in this file, so a compile
 * failure is a can't-happen guard rather than a path with a story -- the
 * caller just stops deriving and ships what it has. */
static bool derived_pass(config_t *c, shader_t *sh, const char *kind,
                         color_t color, uint8_t channels, const char *src) {
  shader_make(sh, kind, color, 128);
  sh->channels = channels;
  char eerr[128] = {0};
  expr_prog_t *p = expr_compile(src, eerr, sizeof eerr);
  if (!p) return false;
  sh->amount_expr = p;
  c->exprs = realloc(c->exprs, (c->nexprs + 1) * sizeof *c->exprs);
  c->exprs[c->nexprs++] = p;
  return true;
}

/* The scrolled default, derived *after* the theme block is read rather than
 * baked from the compiled palette a theme was about to replace. `declared` is
 * whether the config named `scrolled` itself, in which case its chain --
 * including an empty one -- stands.
 *
 * The wash: the scroll indicator's own colour (theme's scroll_bg), weak
 * (about 9%) because scrollback is something you are reading through it.
 *
 * The edge fades: the viewport's top and bottom rows melt towards an edge
 * while there is content past it -- `above`/`below` are the lines hidden past
 * each edge, so `(above > 0)` snaps the top fade off at the top of the
 * buffer: an edge you cannot scroll past renders solid, which is the whole
 * message. (Reaching the bottom ends the state itself, same statement.) Up to
 * four rows a side and never more than a quarter of the viewport each,
 * because two deep fades on a short viewport is every row faded and nothing
 * said. They ride after the wash so they dim washed cells rather than racing
 * it. 60 a row, so the edge row is nearly gone (240) and the melt is a
 * statement rather than a suspicion -- it shipped at three rows of 45 and
 * read as a rendering artefact more than as "there is more this way". */
#define SCROLL_FADE_TOP "(above > 0) * max(0, (min(4, rows / 4) - y) * 60)"
#define SCROLL_FADE_BOTTOM                                                     \
  "(below > 0) * max(0, (y - rows + min(4, rows / 4) + 1) * 60)"

static void apply_scrolled(config_t *c, bool declared) {
  if (declared) return;
  shader_t *chain = c->state_shaders[PSTATE_SCROLLED];
  c->state_n[PSTATE_SCROLLED] = 0;
  shader_make(&chain[0], "tint", c->scroll_bg, 22);
  c->state_n[PSTATE_SCROLLED] = 1;
  if (!derived_pass(c, &chain[1], "dim", (color_t){0}, 0, SCROLL_FADE_TOP))
    return;
  c->state_n[PSTATE_SCROLLED] = 2;
  if (!derived_pass(c, &chain[2], "dim", (color_t){0}, 0, SCROLL_FADE_BOTTOM))
    return;
  c->state_n[PSTATE_SCROLLED] = 3;
}

/* What an unanswered bell does to the frame of the pane that rang: three
 * full-strength blinks over the first 900ms -- the part that catches the eye
 * the moment it rings -- then a slow breathe, so a bell nobody answered stays
 * findable without nagging. The breathe's phase constant starts it at its
 * dimmest (sin(since/8 + 158) is about -1 at since=900), so the hand-off from
 * the last dark blink is a fade-in rather than a pop. */
#define BELL_FLASH_AMOUNT                                                      \
  "since < 900 ? (since / 150 % 2 == 0) * 255 : 120 + sin(since / 8 + 158) / " \
  "3"

/* ...and to its body, for those same 900ms and not a millisecond longer: a
 * shimmer -- one soft diagonal sheen gliding across, the way light crosses
 * something glossy. Deliberately quiet next to the frame: peak 95 where the
 * blinks are 255, with a fade slow enough (since / 64) that the band keeps
 * its strength across the whole sweep and the 900ms cut is what ends it. `x + 2 * y` keeps the band at a true diagonal on
 * a terminal's 1:2 cells, and motion is the whole trick: the eye is pulled by
 * something *moving* long before it is pulled by something bright. The band's
 * width breathes on a sine while it travels -- the falloff slope swings 6±2
 * over roughly one swell per lifetime (sin is -255..255, so /96 is ±2), about
 * ±14 to ±27 columns -- which is what makes it read as a living gleam rather
 * than a ruled stripe. The shimmer is the announcement, the frame the
 * reminder. */
#define BELL_SHIMMER_AMOUNT                                                    \
  "(since < 900) * "                                                           \
  "max(0, 95 - abs(x + 2 * y - since / 4) * (6 + sin(since / 2) / 96) - "      \
  "since / 64)"

/* The shimmer's colour: the bell colour softened halfway towards the theme's
 * default_fg -- light crossing the pane rather than paint laid on it. Derived
 * from both ends of the theme, so it follows either being changed. */
static color_t bell_sheen(const config_t *c) {
  return (color_t){.set = true,
                   .r = (uint8_t)((c->bell.r + c->default_fg.r) / 2),
                   .g = (uint8_t)((c->bell.g + c->default_fg.g) / 2),
                   .b = (uint8_t)((c->bell.b + c->default_fg.b) / 2)};
}

/* Derived like the scrolled wash, and for the same reason: both passes are in
 * theme's `bell` colour, so they are built after the theme is read and a
 * theme moves the mark, the flash and the shimmer together. The frame pass is
 * `channel="fg"`, because a frame's background is the terminal's own default
 * and colouring glyphs is the whole job; the body pass keeps both channels,
 * because most of a body is blank cells whose background is the only thing a
 * shimmer can travel across. It follows `bell_indicator` off -- that knob's
 * promise is "silent *and* invisible" -- and a `states { bell { } }` chain
 * somebody wrote replaces it, like every state. The expressions read `since`,
 * so the frame clock this costs runs only while a rung pane is on screen. */
static void apply_bell(config_t *c, bool declared) {
  if (declared) return;
  c->state_n[PSTATE_BELL] = 0;
  c->chrome_state_n[PSTATE_BELL] = 0;
  if (!c->bell_indicator) return;

  if (derived_pass(c, &c->chrome_state_shaders[PSTATE_BELL][0], "tint", c->bell,
                   SHADE_FG, BELL_FLASH_AMOUNT))
    c->chrome_state_n[PSTATE_BELL] = 1;
  if (derived_pass(c, &c->state_shaders[PSTATE_BELL][0], "tint", bell_sheen(c),
                   0, BELL_SHIMMER_AMOUNT))
    c->state_n[PSTATE_BELL] = 1;
}

void config_defaults(config_t *c) {
  memset(c, 0, sizeof *c);
  c->gap = 1;
  c->gap_aspect = 2;
  c->pad_top = c->pad_right = c->pad_bottom = c->pad_left = 0;
  c->compact = false;
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
  snprintf(c->zoom_mark, sizeof c->zoom_mark, "\u25a1"); /* □ fill the tab */
  snprintf(c->zoom_on_mark, sizeof c->zoom_on_mark,
           "\u25a0");                                      /* ■ ...and back */
  snprintf(c->close_mark, sizeof c->close_mark, "\u2715"); /* ✕ close */
  snprintf(c->min_mark, sizeof c->min_mark, "\u25ac"); /* ▬ into the strip */
  snprintf(c->newtab_mark, sizeof c->newtab_mark, "+");
  c->bell_indicator = true;
  snprintf(c->bell_mark, sizeof c->bell_mark, "[!]");
  c->keep_dead = KEEP_DEAD_COMMANDS;
  /* Gentle: an unfocused pane is one you are still reading half the time.
   * At 60 white text lands on #c3c3c3, which reads as "not this one" without
   * reading as "not available". */
  c->dim_unfocused = 60;
  c->float_shadow = 110;
  c->min_pane_cols = 24;
  c->min_pane_rows = 6;
  c->min_split_cols = 32;
  c->min_split_rows = 8;
  c->scroll_lines = 3;
  /* What every other multiplexer settled on, and what lib-vt does not give you:
   * its own default is 10,000 *bytes*, which measured at 622 lines of an
   * 80-column pane. See config/config.kdl for the ceiling's arithmetic. */
  c->scrollback = 10000;
  c->scrollback_bytes = 16u * 1024 * 1024;
  /* Two levels, because `~/dev/work/api` is as common as `~/dev/api` and a
   * directory that is itself a project is never descended into -- so the second
   * level costs one readdir per non-project directory and never walks a
   * checkout. No roots by default: the feature is dormant until you say where. */
  c->project_depth = 2;
  c->toast_ms = 2500;
  /* Long enough to read as a greeting, short enough never to be in the way
   * -- and any key or click ends it early. */
  c->splash_ms = 900;
  c->hover_delay_ms = 250;
  /* Slow enough to pick a line at one row past the edge, fast enough to cross
   * ten thousand lines of scrollback without regretting the gesture -- and the
   * distance past the edge scales the step, so the hand can hurry it. */
  c->select_scroll_ms = 50;
  c->double_click_ms = 400;
  /* Quotes, brackets, and the punctuation that ends a clause or a list.
   *
   * Deliberately *not* `/ . - _ : = @ # % ? & + ~ $`: a path, a dotted name, a
   * snake_case identifier, a key=value pair, a URL and -- the one that decides
   * it in a terminal -- `src/app.c:1234` are each one thing somebody
   * double-clicked to copy whole. A separator set that breaks those costs a
   * second gesture every time, and `:` is the difference between copying an
   * error location and copying half of one. */
  snprintf(c->word_separators, sizeof c->word_separators, "%s",
           "\"'`()[]{}<>;,|");
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
  c->multi_attach = true;
  c->attach_indicator = true;
  c->size_follows = SIZE_FOLLOWS_ACTIVE;
  c->in_band_shaders = false;
  /* Windows has no VEOF for the line discipline to act on, so ^D there is a
   * key that does nothing until we make it mean something. POSIX already has
   * it, and better than we could. */
#ifdef _WIN32
  c->ctrl_d_exits = true;
#else
  c->ctrl_d_exits = false;
#endif

  c->default_fg = rgb(0xff, 0xff, 0xff);
  c->default_bg = rgb(0x00, 0x00, 0x00);

  /* While a pane is being dragged, everything it could be dropped onto is
   * pushed back so the pane in your hand stands out. */
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
  /* Which states ship a default, and why the line falls where it does, is
   * argued in full in config/config.kdl (the states section) -- one copy of
   * that essay is one that cannot drift. The short form: not-live states
   * (dead, suspended, scrolled) and news (bell) get one; unfocused gets one
   * through the dim_unfocused knob; dragging gets none. All gentle, because
   * every one of them is a pane you still want to read. */
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

  /* Muted blue on neutral dark: the accent has to say "this one" on the
   * focused frame, the guide and the active tab without shouting over the
   * text it frames, and blue is the one hue that reads as focus/selection
   * everywhere else, sits apart from the red-orange reserved for `dead`, and
   * survives the common colour-vision deficiencies doing it. The house pink
   * lives on as contrib/themes/sl0p.kdl, one include away. */
  const color_t accent = rgb(0x7a, 0xa2, 0xf7);
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

  /* After scroll_bg above, and re-derived after a config's theme block: the
   * rationale lives at apply_scrolled. */
  apply_scrolled(c, false);

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

  /* The two colours that mean something rather than match something: a bell
   * is attention, so amber, not the accent -- painted in the focus colour it
   * said nothing -- and a dead pane is warm red-orange. Both sit apart from
   * the blue accent and from each other. */
  c->bell = rgb(0xe0, 0xaf, 0x68);
  c->dead = rgb(0xff, 0x87, 0x5f);

  /* After c->bell above, and re-derived after a config's theme block: the
   * rationale lives at apply_bell. */
  apply_bell(c, false);

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

  /* Quiet on purpose: gray ink on the terminal's own background. The toast
   * colours announce; this one merely mentions. */
  c->attach_fg = dim;
  c->attach_bg = (color_t){0};

  c->prefix_key = GHOSTTY_KEY_A;
  c->prefix_mods = MOD_CTRL;

  /* Enter: the key you press when you want *a* pane and have no opinion about
   * where. The two explicit splits are one key away when you do. */
  bind_add(c, GHOSTTY_KEY_ENTER, 0, ACT_SPLIT, false);
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
  /* `f` is float, not finder. Both wanted the letter; the tie went to the
   * verb -- f is what "float" sounds like, a float toggle is pressed in a
   * flow (lift, look, put back) where a reach hurts, and the finder reads
   * just as naturally as *search* on `s`. */
  bind_add(c, GHOSTTY_KEY_F, 0, ACT_FLOAT, false);
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
  /* The =/+ key grows a focused float and `-` shrinks one (through the
   * split-rows case, where a float's split is a refusal anyway). Grow is
   * bound with and without shift, because whether `+` arrives shifted
   * depends on the outer terminal -- the same deal `?` has with `/` -- and
   * that ambiguity is also why equalize moved instead of sharing the key:
   * two verbs on one key survive only until a terminal drops the shift.
   * Equalize lands on `0`, the digit row it already lives on: 1..9 pick a
   * tab, 0 resets the shares. */
  bind_add(c, GHOSTTY_KEY_EQUAL, 0, ACT_FLOAT_GROW, false);
  bind_add(c, GHOSTTY_KEY_EQUAL, MOD_SHIFT, ACT_FLOAT_GROW, false);
  bind_add(c, GHOSTTY_KEY_DIGIT_0, 0, ACT_EQUALIZE, false);
  /* Shifted beside `f`, the same verb on a fresh thing: `f` floats this
   * pane, `F` opens a new floating shell -- the throwaway terminal. (It
   * spent a day on F12, the IDE key for the same idea, and came back: a
   * chord of the leader and a letter is what every other verb here costs,
   * and the function row is a reach the home row is not.) */
  bind_add(c, GHOSTTY_KEY_F, MOD_SHIFT, ACT_NEW_FLOAT, false);
  /* The leader and the space bar: the biggest key on the keyboard, no modifier,
   * and the one tmux already spends on cycling layouts — so the hand that knows
   * that reaches for the right thing here. Four presses come back round, which
   * is what makes a key this easy to hit the right choice rather than a hazard. */
  bind_add(c, GHOSTTY_KEY_SPACE, 0, ACT_ROTATE_LAYOUT, false);
  /* `>` and `<` are the shifted period and comma, which is how the sheet prints
   * them and how anybody would type them. Pushing a pane one tab along is the
   * cheap version of moving it: no picker, no drag, and the toast says where it
   * went since you do not follow it. */
  bind_add(c, GHOSTTY_KEY_PERIOD, MOD_SHIFT, ACT_PANE_TO_NEXT_TAB, false);
  bind_add(c, GHOSTTY_KEY_COMMA, MOD_SHIFT, ACT_PANE_TO_PREV_TAB, false);
  /* ...and without the shift, because a terminal does not report one for
   * punctuation: `>` arrives as a bare period, the way `?` arrives as a bare
   * slash. The config loader binds both halves for the same reason when you write
   * one of these by hand; the defaults have to do it themselves. The cost is that
   * `.` and `,` are spoken for, which is the cost `/` already pays for `?`. */
  bind_add(c, GHOSTTY_KEY_PERIOD, 0, ACT_PANE_TO_NEXT_TAB, false);
  bind_add(c, GHOSTTY_KEY_COMMA, 0, ACT_PANE_TO_PREV_TAB, false);
  /* `b` for break out, which is the word tmux taught everybody for this. */
  bind_add(c, GHOSTTY_KEY_B, 0, ACT_PANE_TO_NEW_TAB, false);
  bind_add(c, GHOSTTY_KEY_C, 0, ACT_NEW_TAB, false);
  /* Shifted beside `x`, which closes a pane: the same verb on the bigger thing.
   * A tab with four panes in it used to take four presses of `x` and the tab
   * evaporating when the last one went, which is a side effect rather than a
   * verb. */
  bind_add(c, GHOSTTY_KEY_X, MOD_SHIFT, ACT_CLOSE_TAB, false);
  /* Cycling tabs is on tab/shift+tab, not on n/p: `p` is the palette, which is
   * pressed far more often than "the tab before this one" and had the only
   * shifted letter in the defaults. Tab is the key every other tabbed thing
   * cycles with, and it decodes on a plain terminal (`\e[Z` is shift+tab), so
   * this costs nothing on a client without the kitty protocol. */
  bind_add(c, GHOSTTY_KEY_TAB, 0, ACT_NEXT_TAB, false);
  bind_add(c, GHOSTTY_KEY_TAB, MOD_SHIFT, ACT_PREV_TAB, false);
  /* `w` for the projects picker and `W` to write this tab as one's layout: the
   * same shifted-pair shape as `p`/`P`, on the letter people reach for. */
  bind_add(c, GHOSTTY_KEY_W, 0, ACT_WORKSPACES, false);
  bind_add(c, GHOSTTY_KEY_W, MOD_SHIFT, ACT_SAVE_WORKSPACE, false);
  bind_add(c, GHOSTTY_KEY_S, 0, ACT_FINDER, false);
  bind_add(c, GHOSTTY_KEY_P, 0, ACT_PALETTE, false);
  /* Shifted, beside the palette on the same letter: `p` runs a command, `P`
   * tags this pane. A purpose was reachable only from a layout or the socket,
   * which is why panes people arrange by hand have none. */
  bind_add(c, GHOSTTY_KEY_P, MOD_SHIFT, ACT_SET_PURPOSE, false);
  bind_add(c, GHOSTTY_KEY_PAGE_UP, 0, ACT_SCROLL_PAGE_UP, false);
  bind_add(c, GHOSTTY_KEY_PAGE_DOWN, 0, ACT_SCROLL_PAGE_DOWN, false);
  bind_add(c, GHOSTTY_KEY_HOME, 0, ACT_SCROLL_TOP, false);
  bind_add(c, GHOSTTY_KEY_END, 0, ACT_SCROLL_BOTTOM, false);
  bind_add(c, GHOSTTY_KEY_D, 0, ACT_DETACH, false);
  bind_add(c, GHOSTTY_KEY_Q, 0, ACT_QUIT, false);
  for (int i = 0; i < 9; i++)
    bind_add(c, GHOSTTY_KEY_DIGIT_1 + i, 0, (action_t)(ACT_SELECT_TAB_1 + i),
             false);
}

void config_free(config_t *c) {
  for (size_t i = 0; i < c->nexprs; i++) expr_free(c->exprs[i]);
  free(c->exprs);
  free(c->binds);
  free(c->shell);
  free(c->editor);
  free(c->project_layout);
  for (size_t i = 0; i < c->nproject_roots; i++) free(c->project_roots[i]);
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
    /* First in the group: the one to reach for, with the two explicit ones after
     * it for when you mean a particular axis. */
    {ACT_SPLIT, "panes", "split the longer way"},
    {ACT_SPLIT_COLS, "panes", "split into columns"},
    {ACT_SPLIT_ROWS, "panes", "split into rows"},
    {ACT_CLOSE_PANE, "panes", "close this pane"},
    {ACT_RERUN, "panes", "run a finished pane again"},
    {ACT_ZOOM, "panes", "fill the tab with it"},
    {ACT_MINIMIZE, "panes", "put it away in the strip"},
    {ACT_FLOAT, "panes", "float it above the layout"},
    {ACT_NEW_FLOAT, "panes", "a new floating shell"},
    /* Under `panes` because a purpose is a fact about this pane, and the label
     * says "tag" rather than "set purpose": the word people arrive with is the
     * one for what it is *for*, which is finding the pane again later. */
    {ACT_SET_PURPOSE, "panes", "tag it with a purpose"},
    /* Renaming was a double-click and nothing else, which made it the one
     * verb the palette could not reach — exactly what the palette exists to
     * prevent. Unbound by default: the double-click is the fast path, and an
     * action with no key is the palette's whole reason to list it. */
    {ACT_RENAME_PANE, "panes", "rename this pane"},

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
    {ACT_FLOAT_GROW, "size", "grow a floating pane"},
    {ACT_FLOAT_SHRINK, "size", "shrink a floating pane"},
    /* Under `panes`, not `size`: it undoes what the program in the pane did to
     * the pane, which is a fact about that pane and not about the layout. */
    {ACT_CLEAR_SHADERS, "panes", "clear this pane's shaders"},
    /* Under `panes`: what moves is the pane, and the tab it lands in is where it
     * lands. A reader looking for "how do I get this thing out of here" looks up
     * the thing, not the place. */
    {ACT_PANE_TO_NEXT_TAB, "panes", "push it to the tab after"},
    {ACT_PANE_TO_PREV_TAB, "panes", "push it to the tab before"},
    {ACT_PANE_TO_NEW_TAB, "panes", "into a tab of its own"},
    {ACT_ROTATE_LAYOUT, "size", "turn the layout a quarter"},

    {ACT_NEW_TAB, "tabs", "new tab"},
    {ACT_RENAME_TAB, "tabs", "rename this tab"},
    {ACT_CLOSE_TAB, "tabs", "close this tab"},
    {ACT_NEXT_TAB, "tabs", "next tab"},
    {ACT_PREV_TAB, "tabs", "previous tab"},
    {ACT_SELECT_TAB_1, "tabs", "go to that tab"},

    {ACT_SCROLL_UP, "scroll", "up a line"},
    {ACT_SCROLL_DOWN, "scroll", "down a line"},
    {ACT_SCROLL_PAGE_UP, "scroll", "up a page"},
    {ACT_SCROLL_PAGE_DOWN, "scroll", "down a page"},
    {ACT_SCROLL_TOP, "scroll", "to the oldest line"},
    {ACT_SCROLL_BOTTOM, "scroll", "back to the present"},

    /* Their own group: a project is neither a pane nor this session, and the
     * two verbs are the same pair -- go to one, write one down. */
    {ACT_WORKSPACES, "projects", "go to a project"},
    {ACT_SAVE_WORKSPACE, "projects", "save this tab as a layout"},

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
      {GHOSTTY_KEY_SLASH, "/", "?"},
      {GHOSTTY_KEY_BACKSLASH, "\\", "|"},
      {GHOSTTY_KEY_MINUS, "-", "_"},
      {GHOSTTY_KEY_EQUAL, "=", "+"},
      {GHOSTTY_KEY_COMMA, ",", "<"},
      {GHOSTTY_KEY_PERIOD, ".", ">"},
      {GHOSTTY_KEY_SEMICOLON, ";", ":"},
      {GHOSTTY_KEY_QUOTE, "'", "\""},
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
      {GHOSTTY_KEY_ARROW_LEFT, "\u2190"},
      {GHOSTTY_KEY_ARROW_RIGHT, "\u2192"},
      {GHOSTTY_KEY_ARROW_UP, "\u2191"},
      {GHOSTTY_KEY_ARROW_DOWN, "\u2193"},
  };

  for (size_t i = 0; i < sizeof PUNCT / sizeof *PUNCT; i++)
    if (PUNCT[i].key == key) {
      bool shifted = (mods & MOD_SHIFT) != 0;
      snprintf(base, sizeof base, "%s",
               shifted ? PUNCT[i].shifted : PUNCT[i].plain);
      if (shifted) mods &= (uint16_t)~MOD_SHIFT; /* spent on the glyph */
    }
  for (size_t i = 0; i < sizeof ARROWS / sizeof *ARROWS; i++)
    if (ARROWS[i].key == key)
      snprintf(base, sizeof base, "%s", ARROWS[i].glyph);

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
    snprintf(base, sizeof base, "%c",
             (char)('0' + (key - GHOSTTY_KEY_DIGIT_0)));
  if (!base[0]) snprintf(base, sizeof base, "?");

  snprintf(out, cap, "%s%s%s%s", mods & MOD_CTRL ? "C-" : "",
           mods & MOD_ALT ? "M-" : "", mods & MOD_SHIFT ? "S-" : "", base);
}

/* ---- rendering a config back out ----------------------------------------
 *
 * Every knob with its default, as a file you could have written -- the name
 * `config_dump_defaults` is the honest one, and the flag is documented to
 * match: this is the file to *start* from, not a report on the one in effect.
 * Generated rather than kept as a copy on disk: a checked-in "defaults" file is
 * a second source of truth, and it drifts -- ours had already lost four colours
 * by the time anyone noticed.
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
/* A value as a KDL string. `\\` and `"` both end or escape a string, so every
 * value that reaches a dump goes through here: the dump is documented as a file
 * you could have written, and a file that does not parse back is not one. Any
 * setting whose value a person chooses can contain either character -- a shell
 * command with a quoted argument, a word separator set that includes the quote
 * you most want double-click to stop at. */
static void cb_qval(cfgbuf_t *b, const char *val) {
  cb_add(b, "\"");
  for (const char *q = val; q && *q; q++) {
    if (*q == '"' || *q == '\\')
      cb_add(b, "\\%c", *q);
    else
      cb_add(b, "%c", *q);
  }
  cb_add(b, "\"");
}

/* `key "value"` on a line of its own, with an optional trailing comment. */
static void cb_qstr(cfgbuf_t *b, const char *key, const char *val,
                    const char *comment) {
  cb_add(b, "%s ", key);
  cb_qval(b, val);
  if (comment) cb_add(b, "  %s", comment);
  cb_add(b, "\n");
}

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
  if (mods & MOD_CTRL)
    n += (size_t)snprintf(chord + n, sizeof chord - n, "ctrl+");
  if (mods & MOD_ALT)
    n += (size_t)snprintf(chord + n, sizeof chord - n, "alt+");
  if (mods & MOD_SHIFT)
    n += (size_t)snprintf(chord + n, sizeof chord - n, "shift+");
  if (mods & MOD_SUPER)
    n += (size_t)snprintf(chord + n, sizeof chord - n, "super+");

  const char *name = NULL;
  for (size_t i = 0; i < sizeof NAMED_KEYS / sizeof *NAMED_KEYS; i++)
    if (NAMED_KEYS[i].key == key) name = NAMED_KEYS[i].name;

  if (name) {
    snprintf(chord + n, sizeof chord - n, "%s", name);
  } else if (key >= GHOSTTY_KEY_A && key <= GHOSTTY_KEY_Z) {
    snprintf(chord + n, sizeof chord - n, "%c",
             (char)('a' + (key - GHOSTTY_KEY_A)));
  } else if (key >= GHOSTTY_KEY_DIGIT_0 && key <= GHOSTTY_KEY_DIGIT_9) {
    snprintf(chord + n, sizeof chord - n, "%c",
             (char)('0' + (key - GHOSTTY_KEY_DIGIT_0)));
  } else {
    /* The punctuation the parser knows, by the character you press. */
    static const struct {
      int key;
      char ch;
    } PUNCT[] = {
        {GHOSTTY_KEY_EQUAL, '='},         {GHOSTTY_KEY_BRACKET_LEFT, '['},
        {GHOSTTY_KEY_BRACKET_RIGHT, ']'}, {GHOSTTY_KEY_SEMICOLON, ';'},
        {GHOSTTY_KEY_QUOTE, '\''},        {GHOSTTY_KEY_BACKQUOTE, '`'},
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

  cb_qval(b, chord);
}

static void cb_color(cfgbuf_t *b, const char *name, color_t c) {
  cb_add(b, "    %-22s \"#%02x%02x%02x\"\n", name, c.r, c.g, c.b);
}

static void cb_chain(cfgbuf_t *b, const char *indent, const shader_t *sh,
                     size_t n, bool chrome) {
  for (size_t i = 0; i < n; i++) {
    if (!sh[i].kind) continue;
    /* An expression is written back as the line somebody wrote; only a
     * numeric amount is a number. Pretending otherwise is how a dumped
     * config used to lose every expression it contained. */
    if (sh[i].amount_expr && expr_source(sh[i].amount_expr)) {
      cb_add(b, "%s%s amount=", indent, sh[i].kind);
      cb_qval(b, expr_source(sh[i].amount_expr));
    } else {
      cb_add(b, "%s%s amount=%u", indent, sh[i].kind, sh[i].amount);
    }
    if (sh[i].color.set)
      cb_add(b, " color=\"#%02x%02x%02x\"", sh[i].color.r, sh[i].color.g,
             sh[i].color.b);
    if (sh[i].param) cb_add(b, " at=%u", sh[i].param);
    if (chrome) cb_add(b, " where=\"chrome\"");
    if (sh[i].channels == SHADE_FG)
      cb_add(b, " channel=\"fg\"");
    else if (sh[i].channels == SHADE_BG)
      cb_add(b, " channel=\"bg\"");
    cb_add(b, "\n");
  }
}

/* Whether a state's chain is still the one the loader derives rather than one
 * somebody wrote: `unfocused` from the dim_unfocused knob, `scrolled` from
 * theme's scroll_bg. Field-wise rather than memcmp, because a struct copy
 * carries padding nobody initialised. */
static bool state_is_derived(const config_t *c, int st) {
  switch (st) {
  /* bell and scrolled carry compiled expressions, which cannot be compared
   * structurally -- but they also cannot be written by a config, so the
   * declared flags (persisted across an include chain) are the whole test. */
  case PSTATE_BELL: return !c->bell_declared;
  case PSTATE_SCROLLED: return !c->scrolled_declared;
  /* unfocused has no flag -- the knob is re-applied per file -- so it is
   * compared against what the knob would build: exactly one plain dim at the
   * knob's strength, or nothing when the knob is 0. */
  case PSTATE_UNFOCUSED: {
    if (c->chrome_state_n[st]) return false;
    if (!c->dim_unfocused) return c->state_n[st] == 0;
    if (c->state_n[st] != 1) return false;
    const shader_t *got = &c->state_shaders[st][0];
    return got->kind && strcmp(got->kind, "dim") == 0 && !got->amount_expr &&
           got->amount == c->dim_unfocused && !got->color.set && !got->param &&
           !got->channels;
  }
  default: return false;
  }
}

char *config_render(const config_t *c) {
  cfgbuf_t b = {0};

  cb_add(&b, "// slosh config, as it currently stands.\n");
  cb_add(&b, "//\n");
  cb_add(&b, "// Written by `slosh --dump-config`, so every value here is\n");
  cb_add(&b, "// the one in force rather than one somebody typed up. Delete\n");
  cb_add(&b, "// anything you do not want to pin; what is missing is a\n");
  cb_add(&b, "// default, and defaults are allowed to improve.\n");
  cb_add(&b, "//\n");
  cb_add(&b,
         "// The commented reference -- what each setting is for, and why\n");
  cb_add(&b, "// it exists -- is config/config.kdl in the source tree.\n\n");

  cb_add(&b, "// ---- geometry ----\n");
  cb_add(&b, "gap %u\n", c->gap);
  cb_add(&b,
         "gap_aspect %u          // columns per row, so a gap looks square\n",
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
  cb_add(&b, "compact %s            // shared 1-cell borders instead of gaps\n",
         yesno(c->compact));
  cb_add(&b, "title_align \"%s\"\n",
         c->title_align == ALIGN_LEFT    ? "left"
         : c->title_align == ALIGN_RIGHT ? "right"
                                         : "center");
  cb_add(&b, "title_inset %u\n", c->title_inset);
  cb_add(&b, "min_pane cols=%u rows=%u   // below this a pane collapses\n",
         c->min_pane_cols, c->min_pane_rows);
  cb_add(&b,
         "min_split cols=%u rows=%u  // below this a split is not offered\n",
         c->min_split_cols, c->min_split_rows);

  cb_add(&b, "\n// ---- what is on screen ----\n");
  cb_add(&b, "status_bar %s          // the strip along the top\n",
         yesno(c->status_bar));
  cb_add(&b, "status_line %s         // the line along the bottom\n",
         yesno(c->status_line));
  cb_add(&b, "status_pad %u\n", c->status_pad);
  cb_add(&b,
         "hints %s               // what the pointer is on, in the middle\n",
         yesno(c->hints));
  cb_add(&b,
         "version_banner %s      // ...and which build this is, when idle\n",
         yesno(c->version_banner));
  cb_add(&b, "pane_buttons %s        // the marks in a frame's top-right\n",
         yesno(c->pane_buttons));
  cb_add(&b, "bell_indicator %s\n", yesno(c->bell_indicator));
  cb_qstr(&b, "zoom_mark", c->zoom_mark, NULL);
  cb_qstr(&b, "zoom_on_mark", c->zoom_on_mark, NULL);
  cb_qstr(&b, "close_mark", c->close_mark, NULL);
  cb_qstr(&b, "min_mark", c->min_mark, NULL);
  cb_qstr(&b, "newtab_mark", c->newtab_mark, NULL);
  cb_qstr(&b, "bell_mark", c->bell_mark, NULL);

  cb_add(&b, "\n// ---- behaviour ----\n");
  cb_add(&b, "focus_follows_mouse %s\n", yesno(c->focus_follows_mouse));
  cb_add(&b,
         "multi_attach %s        // several terminals at once; off: a new "
         "client displaces the old\n",
         yesno(c->multi_attach));
  cb_add(&b,
         "attach_indicator %s    // top-right tag when others are watching\n",
         yesno(c->attach_indicator));
  cb_add(&b, "size_follows \"%s\"  // or \"smallest\" / \"largest\"\n",
         c->size_follows == SIZE_FOLLOWS_SMALLEST  ? "smallest"
         : c->size_follows == SIZE_FOLLOWS_LARGEST ? "largest"
                                                   : "active");
  cb_add(&b, "in_band_shaders %s\n", yesno(c->in_band_shaders));
  cb_add(&b,
         "ctrl_d_exits %s        // ^D at an idle prompt exits the shell; "
         "default on Windows only\n",
         yesno(c->ctrl_d_exits));
  cb_add(&b, "scroll_lines %u\n", c->scroll_lines);
  cb_add(&b,
         "scrollback %zu           // lines of history per pane; 0 for none\n",
         c->scrollback);
  cb_add(
      &b,
      "scrollback_bytes %zu  // the ceiling that count runs into; 0 for none\n",
      c->scrollback_bytes);
  cb_add(&b, "toast_ms %u\n", c->toast_ms);
  cb_add(&b, "splash_ms %u          // the logo, on attach; 0 for never\n",
         c->splash_ms);
  cb_add(&b, "hover_delay_ms %u\n", c->hover_delay_ms);
  cb_add(&b,
         "select_scroll_ms %u      // drag past an edge: ms per scroll step\n",
         c->select_scroll_ms);
  cb_add(&b, "double_click_ms %u\n", c->double_click_ms);
  cb_qstr(&b, "word_separators", c->word_separators,
          "// what a double-click's word stops at");
  cb_add(&b, "anim_ms %u             // frame clock while a shader animates\n",
         c->anim_ms);
  cb_add(&b, "modal_scrim %u         // how far a modal pushes the rest back\n",
         c->modal_scrim);
  cb_add(&b,
         "dim_unfocused %u       // ...and how far the panes you are not in\n",
         c->dim_unfocused);
  cb_add(&b,
         "float_shadow %u       // the shade a floating pane casts; 0 for "
         "none\n",
         c->float_shadow);
  cb_add(&b,
         "keep_dead \"%s\"  // which dead panes stay: commands, all, none\n",
         c->keep_dead == KEEP_DEAD_ALL    ? "all"
         : c->keep_dead == KEEP_DEAD_NONE ? "none"
                                          : "commands");
  if (c->shell)
    cb_qstr(&b, "shell", c->shell, NULL);
  else
    cb_add(&b, "// shell \"/bin/zsh\"     // unset: $SHELL\n");
  if (c->editor)
    cb_qstr(&b, "editor", c->editor, NULL);
  else
    cb_add(&b, "// editor \"nvim\"        // unset: $EDITOR, then vi\n");
  if (c->nproject_roots) {
    cb_add(&b, "project_roots");
    for (size_t i = 0; i < c->nproject_roots; i++) {
      cb_add(&b, " ");
      cb_qval(&b, c->project_roots[i]);
    }
    cb_add(&b, " depth=%d\n", c->project_depth);
  } else {
    cb_add(&b, "// project_roots \"~/dev\" \"~/work\" depth=%d\n",
           c->project_depth);
  }
  if (c->project_layout)
    cb_qstr(&b, "project_layout", c->project_layout, NULL);
  else
    cb_add(&b, "// project_layout \"~/.config/slosh/project.layout\"\n");

  cb_add(&b, "\n// ---- colour ----\ntheme {\n");
  for (size_t i = 0; i < sizeof THEME_COLORS / sizeof *THEME_COLORS; i++)
    cb_color(&b, THEME_COLORS[i].name, *THEME_COLOR((config_t *)c, i));
  cb_add(&b, "}\n");

  /* Written even when empty: an empty block says "this exists and you have
   * none", where nothing at all says "we forgot to tell you". Chrome passes
   * live in the same blocks, marked, because that is how they are written. */
  cb_add(&b, "\n// ---- colour passes over every pane ----\n");
  cb_add(
      &b,
      "// (contrib/shaders has thirty-odd to paste; contrib/shadertoy.html\n");
  cb_add(&b,
         "//  previews them. where=\"chrome\" runs a pass over the frame\n");
  cb_add(&b, "//  instead of the contents)\nshaders {\n");
  cb_chain(&b, "    ", c->shaders, c->nshaders, false);
  cb_chain(&b, "    ", c->chrome_shaders, c->nchrome_shaders, true);
  cb_add(&b, "}\n");

  cb_add(&b,
         "\n// ---- what a pane looks like in a given state ----\nstates {\n");
  for (int st = 0; st < PSTATE_COUNT; st++) {
    /* The derived states are written as comments while they still match
     * their derivation: declaring a state replaces its default outright, so a
     * dump that declared these would pin what is meant to keep following the
     * dim_unfocused knob and the theme -- and `C-a e` seeds a fresh config
     * with exactly this text, which is the moment nobody has declared
     * anything yet. The round trip is unharmed, because loading the comment
     * derives the identical chain. A chain somebody replaced no longer
     * matches, and is written out declared, as it must be. */
    if (state_is_derived(c, st)) {
      const char *why =
          st == PSTATE_BELL       ? "a body shimmer, and a frame "
                                    "blink-then-breathe, in theme's bell "
                                    "colour"
          : st == PSTATE_SCROLLED ? "a wash of theme's scroll_bg, and edge "
                                    "fades where more content is"
                                  : "the dim_unfocused knob above";
      cb_add(&b, "    // %s {   // derived: %s\n",
             pane_state_name((pane_state_t)st), why);
      cb_chain(&b, "    //     ", c->state_shaders[st], c->state_n[st], false);
      cb_chain(&b, "    //     ", c->chrome_state_shaders[st],
               c->chrome_state_n[st], true);
      if (st == PSTATE_BELL && !c->state_n[st] && !c->chrome_state_n[st])
        cb_add(&b, "    //     // nothing: bell_indicator is false\n");
      cb_add(&b, "    // }\n");
      continue;
    }
    cb_add(&b, "    %s {\n", pane_state_name((pane_state_t)st));
    cb_chain(&b, "        ", c->state_shaders[st], c->state_n[st], false);
    cb_chain(&b, "        ", c->chrome_state_shaders[st], c->chrome_state_n[st],
             true);
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
  const char *explicit_ = getenv("SLOSH_CONFIG");
  if (explicit_ && *explicit_) {
    snprintf(path, sizeof path, "%s", explicit_);
    return path;
  }
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg)
    snprintf(path, sizeof path, "%s/slosh/config.kdl", xdg);
  else {
    const char *home = getenv("HOME");
    snprintf(path, sizeof path, "%s/.config/slosh/config.kdl",
             home ? home : ".");
  }
  return path;
}

/* How deep a chain of includes may go. A depth limit rather than a set of files
 * already seen, because the two failures it has to stop are the same failure:
 * a config that includes itself and a config eleven files deep are both a
 * mistake, and the message says which one you have. */
#define INCLUDE_MAX_DEPTH 8

/* Every node this file reads at the top level, so that one it does *not* read can
 * be said out loud. Without this a whole document of the wrong shape -- a layout
 * handed to `--check`, or `include`d into a config -- passes silently, because a
 * loader that only asks for what it knows never notices what it was given. A
 * mistyped setting did the same thing quietly.
 *
 * Kept honest by `tests/test_config.py`, which greps this file for the names it
 * actually asks `kdl_child(root, ...)` for and fails if one is missing here. */
static const char *const KNOWN_TOP[] = {
    "include", /* read before the rest, but a name this file understands */
    "anim_ms",
    "attach_indicator",
    "bell_indicator",
    "bell_mark",
    "close_mark",
    "compact",
    "ctrl_d_exits",
    "dim_unfocused",
    "double_click_ms",
    "word_separators",
    "editor",
    "float_shadow",
    "focus_follows_mouse",
    "gap",
    "gap_aspect",
    "hints",
    "hover_delay_ms",
    "in_band_shaders",
    "keep_dead",
    "keys",
    "min_mark",
    "min_pane",
    "multi_attach",
    "min_split",
    "modal_scrim",
    "newtab_mark",
    "padding",
    "pane_buttons",
    "project_layout",
    "project_roots",
    "rounded",
    "scroll_lines",
    "scrollback",
    "scrollback_bytes",
    "select_scroll_ms",
    "shaders",
    "shell",
    "size_follows",
    "splash_ms",
    "states",
    "status_bar",
    "status_line",
    "status_pad",
    "theme",
    "title_align",
    "title_inset",
    "toast_ms",
    "version_banner",
    "zoom_mark",
    "zoom_on_mark",
};

/* Whether a name is one of this file's settings. Public so that the *layout* loader
 * can tell somebody they handed it a config -- the two documents share a syntax, so
 * telling them apart has to come from one list rather than two guesses. */
bool config_is_setting(const char *name) {
  if (!name) return false;
  for (size_t i = 0; i < sizeof KNOWN_TOP / sizeof *KNOWN_TOP; i++)
    if (strcmp(name, KNOWN_TOP[i]) == 0) return true;
  return false;
}

/* Named for what a stray node probably *is* rather than only for what it is not: a
 * `layout` at the top of a document is somebody's session file, and the mistake
 * worth naming is the one people make. */
static void complain_unknown_top(config_t *c, const kdl_node_t *root, char *err,
                                 size_t errcap) {
  for (size_t i = 0; i < root->nkids; i++) {
    const kdl_node_t *n = root->kids[i];
    if (!n || !n->name) continue;
    if (config_is_setting(n->name)) continue;
    if (strcmp(n->name, "layout") == 0)
      complain(c, err, errcap, n->line,
               "this is a layout, not a config: `slosh --layout` reads those");
    else
      complain(c, err, errcap, n->line, "unknown setting: %s", n->name);
  }
}

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
  if (path_is_absolute(r)) {
    snprintf(out, cap, "%s", r);
    return;
  }
  char dir[512];
  snprintf(out, cap, "%s/%s", path_dir(base_file, dir, sizeof dir), r);
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
static void apply_includes(config_t *c, const kdl_node_t *root,
                           const char *path, int depth, char *err,
                           size_t errcap) {
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
      if (ierr[0] && err && errcap && !err[0])
        snprintf(err, errcap, "%s", ierr);
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
  /* ...which includes "I have no idea what this document is", so it has to be
   * said after the file is known and not before. */
  complain_unknown_top(c, root, err, errcap);

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
        c->pad_top = c->pad_right = c->pad_bottom = c->pad_left =
            (uint16_t)v[0];
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
                 "top right bottom left), not %zu",
                 pn->nargs);
      }
    }
  }
  c->rounded = kdl_arg_bool(kdl_child(root, "rounded"), 0, c->rounded);
  c->compact = kdl_arg_bool(kdl_child(root, "compact"), 0, c->compact);
  c->status_bar = kdl_arg_bool(kdl_child(root, "status_bar"), 0, c->status_bar);
  c->status_line =
      kdl_arg_bool(kdl_child(root, "status_line"), 0, c->status_line);
  c->status_pad =
      (uint16_t)kdl_arg_int(kdl_child(root, "status_pad"), 0, c->status_pad);
  c->focus_follows_mouse = kdl_arg_bool(kdl_child(root, "focus_follows_mouse"),
                                        0, c->focus_follows_mouse);
  c->multi_attach =
      kdl_arg_bool(kdl_child(root, "multi_attach"), 0, c->multi_attach);
  c->attach_indicator =
      kdl_arg_bool(kdl_child(root, "attach_indicator"), 0, c->attach_indicator);
  const char *sizef = kdl_arg(kdl_child(root, "size_follows"), 0, NULL);
  if (sizef) {
    if (strcmp(sizef, "active") == 0)
      c->size_follows = SIZE_FOLLOWS_ACTIVE;
    else if (strcmp(sizef, "smallest") == 0)
      c->size_follows = SIZE_FOLLOWS_SMALLEST;
    else if (strcmp(sizef, "largest") == 0)
      c->size_follows = SIZE_FOLLOWS_LARGEST;
    else
      complain(c, err, errcap, 0,
               "size_follows wants active, smallest or largest, not \"%s\"",
               sizef);
  }
  c->in_band_shaders =
      kdl_arg_bool(kdl_child(root, "in_band_shaders"), 0, c->in_band_shaders);
  c->ctrl_d_exits =
      kdl_arg_bool(kdl_child(root, "ctrl_d_exits"), 0, c->ctrl_d_exits);

  const char *align = kdl_arg(kdl_child(root, "title_align"), 0, NULL);
  if (align) {
    if (strcmp(align, "left") == 0)
      c->title_align = ALIGN_LEFT;
    else if (strcmp(align, "right") == 0)
      c->title_align = ALIGN_RIGHT;
    else
      c->title_align = ALIGN_CENTER;
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
      if (!strcmp(kd, "all") || !strcmp(kd, "true"))
        c->keep_dead = KEEP_DEAD_ALL;
      else if (!strcmp(kd, "none") || !strcmp(kd, "false"))
        c->keep_dead = KEEP_DEAD_NONE;
      else if (!strcmp(kd, "commands"))
        c->keep_dead = KEEP_DEAD_COMMANDS;
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
    c->min_split_cols = (uint16_t)kdl_prop_int(mins, "cols", c->min_split_cols);
    c->min_split_rows = (uint16_t)kdl_prop_int(mins, "rows", c->min_split_rows);
  }

  const kdl_node_t *minp = kdl_child(root, "min_pane");
  if (minp) {
    c->min_pane_cols = (uint16_t)kdl_prop_int(minp, "cols", c->min_pane_cols);
    c->min_pane_rows = (uint16_t)kdl_prop_int(minp, "rows", c->min_pane_rows);
  }
  c->scroll_lines = (uint16_t)kdl_arg_int(kdl_child(root, "scroll_lines"), 0,
                                          c->scroll_lines);
  /* Read as a `long` so a negative is *refused* rather than wrapping into a
   * limit of eighteen quintillion lines, which is how "unlimited" gets into a
   * program by accident. There is no unlimited on purpose: with 64 panes it is a
   * memory leak with a friendly name. */
  const kdl_node_t *sbn = kdl_child(root, "scrollback");
  if (sbn) {
    long v = kdl_arg_int(sbn, 0, (long)c->scrollback);
    if (v < 0)
      complain(c, err, errcap, sbn->line,
               "scrollback is a number of lines, 0 for none, not %ld", v);
    else
      c->scrollback = (size_t)v;
  }
  const kdl_node_t *sbb = kdl_child(root, "scrollback_bytes");
  if (sbb) {
    long v = kdl_arg_int(sbb, 0, (long)c->scrollback_bytes);
    if (v < 0)
      complain(c, err, errcap, sbb->line,
               "scrollback_bytes is a byte ceiling, 0 for none, not %ld", v);
    else
      c->scrollback_bytes = (size_t)v;
  }
  c->toast_ms =
      (uint16_t)kdl_arg_int(kdl_child(root, "toast_ms"), 0, c->toast_ms);
  c->splash_ms =
      (uint16_t)kdl_arg_int(kdl_child(root, "splash_ms"), 0, c->splash_ms);
  c->hover_delay_ms = (uint16_t)kdl_arg_int(kdl_child(root, "hover_delay_ms"),
                                            0, c->hover_delay_ms);
  c->select_scroll_ms = (uint16_t)kdl_arg_int(
      kdl_child(root, "select_scroll_ms"), 0, c->select_scroll_ms);
  c->double_click_ms = (uint16_t)kdl_arg_int(kdl_child(root, "double_click_ms"),
                                             0, c->double_click_ms);
  const char *ws = kdl_arg(kdl_child(root, "word_separators"), 0, NULL);
  if (ws) snprintf(c->word_separators, sizeof c->word_separators, "%s", ws);
  c->anim_ms = (uint16_t)kdl_arg_int(kdl_child(root, "anim_ms"), 0, c->anim_ms);
  {
    long v = kdl_arg_int(kdl_child(root, "modal_scrim"), 0, c->modal_scrim);
    c->modal_scrim = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
  }
  {
    long v = kdl_arg_int(kdl_child(root, "float_shadow"), 0, c->float_shadow);
    c->float_shadow = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
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

  /* Where projects live. Several roots because people have `~/dev` and `~/work`,
   * and a `depth` property rather than a second node because it is one small
   * fixed record -- the same shape `min_pane cols=24 rows=6` uses.
   *
   * Naming the node at all replaces the roots it inherited, the way a `shaders`
   * block does: a list you can only add to is a list you cannot correct. */
  const kdl_node_t *proots = kdl_child(root, "project_roots");
  if (proots) {
    for (size_t i = 0; i < c->nproject_roots; i++) free(c->project_roots[i]);
    c->nproject_roots = 0;
    for (size_t i = 0; i < proots->nargs; i++) {
      if (c->nproject_roots >= PROJECT_ROOTS_MAX) {
        complain(c, err, errcap, proots->line,
                 "project_roots takes at most %d directories",
                 PROJECT_ROOTS_MAX);
        break;
      }
      c->project_roots[c->nproject_roots++] = strdup(proots->args[i]);
    }
    if (!proots->nargs)
      complain(c, err, errcap, proots->line,
               "project_roots takes one or more directories");
    long d = kdl_prop_int(proots, "depth", c->project_depth);
    if (d < 1 || d > 8)
      complain(c, err, errcap, proots->line,
               "depth is between 1 and 8, not %ld", d);
    else
      c->project_depth = (int)d;
  }

  const char *play = kdl_arg(kdl_child(root, "project_layout"), 0, NULL);
  if (play) {
    free(c->project_layout);
    c->project_layout = strdup(play);
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
      if (st == PSTATE_SCROLLED) c->scrolled_declared = true;
      if (st == PSTATE_BELL) c->bell_declared = true;
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
  /* After the theme, so the wash follows whatever scroll_bg the theme just
   * chose -- a config that wrote its own `scrolled` chain keeps it, whether
   * the chain was written here or in a file this one included. */
  apply_scrolled(c, c->scrolled_declared);
  apply_bell(c, c->bell_declared);

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
