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

static int key_from_char(char c) {
  if (c >= 'a' && c <= 'z') return GHOSTTY_KEY_A + (c - 'a');
  if (c >= 'A' && c <= 'Z') return GHOSTTY_KEY_A + (c - 'A');
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
    default: return GHOSTTY_KEY_UNIDENTIFIED;
  }
}

bool config_parse_chord(const char *text, int *out_key, uint16_t *out_mods) {
  uint16_t mods = 0;
  const char *p = text;
  for (;;) {
    if (strncmp(p, "ctrl+", 5) == 0) { mods |= MOD_CTRL; p += 5; }
    else if (strncmp(p, "alt+", 4) == 0) { mods |= MOD_ALT; p += 4; }
    else if (strncmp(p, "shift+", 6) == 0) { mods |= MOD_SHIFT; p += 6; }
    else if (strncmp(p, "super+", 6) == 0) { mods |= MOD_SUPER; p += 6; }
    else break;
  }
  if (!*p) return false;

  for (size_t i = 0; i < sizeof NAMED_KEYS / sizeof *NAMED_KEYS; i++)
    if (strcmp(p, NAMED_KEYS[i].name) == 0) {
      *out_key = NAMED_KEYS[i].key;
      *out_mods = mods;
      return true;
    }

  if (p[1] != 0) return false; /* not a name and not one character */
  int key = key_from_char(*p);
  if (key == GHOSTTY_KEY_UNIDENTIFIED) return false;
  *out_key = key;
  *out_mods = mods;
  return true;
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

static const char *const PSTATE_NAMES[PSTATE_COUNT] = {
    "dragging", "drop_hover", "drop_target", "dead", "suspended",
    "bell",     "scrolled",   "unfocused",
};

const char *pane_state_name(pane_state_t s) {
  return s < PSTATE_COUNT ? PSTATE_NAMES[s] : "";
}

/* Reads the children of `node` as shader chains: the ones that run over a
 * pane's contents and the ones that run over its frame, routed by each entry's
 * `where` (default "content"). One parser, because a chrome pass is not a
 * different kind of shader — it is the same chain run over a different rect,
 * and a second parser would be a second place for `amount` to mean something
 * slightly else. Counts are set, not added to: naming a block replaces it. */
static void parse_shader_list(config_t *c, const kdl_node_t *node,
                              shader_t *content, size_t *ncontent,
                              shader_t *chrome, size_t *nchrome, char *err,
                              size_t errcap) {
  *ncontent = 0;
  *nchrome = 0;
  for (size_t i = 0; i < node->nkids; i++) {
    const kdl_node_t *k = node->kids[i];
    if (!k || !k->name) continue;

    /* Where this pass runs. Dropped rather than defaulted when the word is not
     * one we know, for the same reason a bad `amount` is: the entry says what
     * it wants and we cannot do it, and running it over the contents because
     * "chrom" was a typo would be a surprise nobody asked for. */
    const char *where = kdl_prop(k, "where", "content");
    bool on_chrome = strcmp(where, "chrome") == 0;
    if (!on_chrome && strcmp(where, "content") != 0) {
      if (err && !err[0])
        snprintf(err, errcap, "bad where for %s: %s (content or chrome)",
                 k->name, where);
      continue;
    }
    shader_t *out = on_chrome ? chrome : content;
    size_t *n = on_chrome ? nchrome : ncontent;
    if (*n >= SHADE_MAX) continue;

    /* Which of the cell's two colours it may touch. `fg` is what a *border*
     * flash wants: a frame's background is usually the terminal's own default,
     * and mixing that towards a colour turns a recoloured glyph into a painted
     * rectangle. Refused the same way a bad `where` is, and for the same
     * reason. */
    const char *chan = kdl_prop(k, "channel", "both");
    uint8_t channels = SHADE_BOTH;
    if (strcmp(chan, "fg") == 0) channels = SHADE_FG;
    else if (strcmp(chan, "bg") == 0) channels = SHADE_BG;
    else if (strcmp(chan, "both") != 0) {
      if (err && !err[0])
        snprintf(err, errcap, "bad channel for %s: %s (fg, bg or both)",
                 k->name, chan);
      continue;
    }

    /* `amount` is a number, or an expression that produces one per cell.
     * Same key either way: `amount=90` and `amount="(y % 2) * 40"` are the
     * same idea, one of them constant, and the compiler folds a constant
     * expression back to a number so nothing downstream can tell. */
    expr_prog_t *aexpr = NULL;
    long amount = 128;
    const char *as = kdl_prop(k, "amount", NULL);
    if (as) {
      char *end = NULL;
      long v = strtol(as, &end, 10);
      while (end && (*end == ' ' || *end == '\t')) end++;
      if (end && !*end) {
        amount = v;
      } else {
        char eerr[128] = {0};
        aexpr = expr_compile(as, eerr, sizeof eerr);
        if (!aexpr) {
          /* The shader is dropped, not run at its default strength: an
           * expression that did not compile leaves the strength *unknown*,
           * and half-dimming a pane is a worse answer to that than doing
           * nothing and saying why. */
          if (err && !err[0])
            snprintf(err, errcap, "bad amount for %s: %s", k->name, eerr);
          continue;
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

    color_t col = c->frame_focus; /* a sensible default for `ruler` */
    const char *cs = kdl_prop(k, "color", NULL);
    if (cs && !parse_color(cs, &col) && err && !err[0])
      snprintf(err, errcap, "bad colour for shader %s: %s", k->name, cs);

    shader_t *slot = &out[*n];
    if (!shader_make_p(slot, k->name, col, (uint8_t)amount, (uint16_t)param)) {
      if (err && !err[0]) snprintf(err, errcap, "unknown shader: %s", k->name);
      expr_free(aexpr);
      continue;
    }
    slot->channels = channels;
    slot->amount_expr = aexpr;
    if (aexpr) {
      c->exprs = realloc(c->exprs, (c->nexprs + 1) * sizeof *c->exprs);
      c->exprs[c->nexprs++] = aexpr; /* the config owns every program */
    }
    (*n)++;
  }
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
  c->pad = 0;
  c->rounded = true;
  c->title_align = ALIGN_CENTER;
  c->title_inset = 2;
  c->hints = true;
  c->version_banner = true;
  c->pane_buttons = true;
  snprintf(c->zoom_mark, sizeof c->zoom_mark, "#");
  snprintf(c->zoom_on_mark, sizeof c->zoom_on_mark, "*");
  snprintf(c->close_mark, sizeof c->close_mark, "x");
  snprintf(c->min_mark, sizeof c->min_mark, "_");
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
static void cb_chord(cfgbuf_t *b, int key, uint16_t mods) {
  char chord[24];
  config_chord_name(key, mods, chord, sizeof chord);
  cb_add(b, "\"");
  for (const char *p = chord; *p; p++) {
    if (*p == '"' || *p == '\\') cb_add(b, "\\%c", *p);
    else cb_add(b, "%c", *p);
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
  cb_add(&b, "padding %u\n", c->pad);
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

bool config_load(config_t *c, const char *path, char *err, size_t errcap) {
  if (err && errcap) err[0] = 0;
  kdl_node_t *root = kdl_parse_file(path, err, errcap);
  if (!root) return false; /* defaults stand; the caller reports why */

  c->gap = (uint16_t)kdl_arg_int(kdl_child(root, "gap"), 0, c->gap);
  c->gap_aspect =
      (uint16_t)kdl_arg_int(kdl_child(root, "gap_aspect"), 0, c->gap_aspect);
  c->pad = (uint16_t)kdl_arg_int(kdl_child(root, "padding"), 0, c->pad);
  c->rounded = kdl_arg_bool(kdl_child(root, "rounded"), 0, c->rounded);
  c->status_bar = kdl_arg_bool(kdl_child(root, "status_bar"), 0, c->status_bar);
  c->status_line =
      kdl_arg_bool(kdl_child(root, "status_line"), 0, c->status_line);
  c->status_pad =
      (uint16_t)kdl_arg_int(kdl_child(root, "status_pad"), 0, c->status_pad);
  c->focus_follows_mouse = kdl_arg_bool(kdl_child(root, "focus_follows_mouse"), 0,
                                        c->focus_follows_mouse);

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
      else if (err && !err[0])
        snprintf(err, errcap, "keep_dead: %s (want commands, all or none)", kd);
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
    if (lerr[0] && err && !err[0]) snprintf(err, errcap, "%s", lerr);
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
        if (err && !err[0]) snprintf(err, errcap, "unknown pane state: %s", k->name);
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
      if (!parse_color(v, THEME_COLOR(c, i)) && err && !err[0])
        snprintf(err, errcap, "bad colour for %s: %s", THEME_COLORS[i].name, v);
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
      } else if (err && !err[0]) {
        snprintf(err, errcap, "bad prefix: %s", pfx);
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
        if (!chord || !act || !config_parse_chord(chord, &k, &m)) {
          if (err && !err[0])
            snprintf(err, errcap, "line %d: bad binding", b->line);
          continue;
        }
        action_t a = action_by_name(act);
        if (a == ACT_NONE && strcmp(act, "none") != 0) {
          if (err && !err[0])
            snprintf(err, errcap, "line %d: unknown action %s", b->line, act);
          continue;
        }
        bind_add(c, k, m, a, direct);
      }
    }
  }

  kdl_free(root);
  return true;
}
