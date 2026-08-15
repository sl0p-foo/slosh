#define _GNU_SOURCE
#include "config.h"

#include <ghostty/vt.h>
#include <stdio.h>
#include <stdlib.h>
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
    {"scroll-up", ACT_SCROLL_UP},     {"scroll-down", ACT_SCROLL_DOWN},
    {"scroll-page-up", ACT_SCROLL_PAGE_UP},
    {"scroll-page-down", ACT_SCROLL_PAGE_DOWN},
    {"scroll-top", ACT_SCROLL_TOP},   {"scroll-bottom", ACT_SCROLL_BOTTOM},
    {"new-tab", ACT_NEW_TAB},         {"next-tab", ACT_NEXT_TAB},
    {"prev-tab", ACT_PREV_TAB},       {"finder", ACT_FINDER},
    {"detach", ACT_DETACH},           {"quit", ACT_QUIT},
    {"literal-prefix", ACT_LITERAL_PREFIX},
    {"help", ACT_HELP},
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
    "scrolled",  "unfocused",
};

const char *pane_state_name(pane_state_t s) {
  return s < PSTATE_COUNT ? PSTATE_NAMES[s] : "";
}

/* Reads the children of `node` as a shader chain into `out` (SHADE_MAX slots),
 * returning how many were understood. */
static size_t parse_shader_list(config_t *c, const kdl_node_t *node,
                                shader_t *out, char *err, size_t errcap) {
  size_t n = 0;
  for (size_t i = 0; i < node->nkids && n < SHADE_MAX; i++) {
    const kdl_node_t *k = node->kids[i];
    if (!k || !k->name) continue;

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

    if (!shader_make_p(&out[n], k->name, col, (uint8_t)amount,
                       (uint16_t)param)) {
      if (err && !err[0]) snprintf(err, errcap, "unknown shader: %s", k->name);
      expr_free(aexpr);
      continue;
    }
    out[n].amount_expr = aexpr;
    if (aexpr) {
      c->exprs = realloc(c->exprs, (c->nexprs + 1) * sizeof *c->exprs);
      c->exprs[c->nexprs++] = aexpr; /* the config owns every program */
    }
    n++;
  }
  return n;
}

static void bind_add(config_t *c, int key, uint16_t mods, action_t action) {
  for (size_t i = 0; i < c->nbinds; i++)
    if (c->binds[i].key == key && c->binds[i].mods == mods) {
      c->binds[i].action = action; /* a later binding replaces an earlier one */
      return;
    }
  c->binds = realloc(c->binds, (c->nbinds + 1) * sizeof *c->binds);
  c->binds[c->nbinds++] = (binding_t){key, mods, action};
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
  c->pane_buttons = true;
  snprintf(c->zoom_mark, sizeof c->zoom_mark, "#");
  snprintf(c->zoom_on_mark, sizeof c->zoom_on_mark, "*");
  snprintf(c->close_mark, sizeof c->close_mark, "x");
  snprintf(c->min_mark, sizeof c->min_mark, "_");
  snprintf(c->newtab_mark, sizeof c->newtab_mark, "+");
  c->bell_indicator = true;
  snprintf(c->bell_mark, sizeof c->bell_mark, "\u2022");
  c->keep_dead = true;
  c->min_pane_cols = 24;
  c->min_pane_rows = 6;
  c->min_split_cols = 32;
  c->min_split_rows = 8;
  c->scroll_lines = 3;
  c->toast_ms = 2500;
  c->hover_delay_ms = 250;
  c->double_click_ms = 400;
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

  bind_add(c, GHOSTTY_KEY_BACKSLASH, 0, ACT_SPLIT_COLS);
  bind_add(c, GHOSTTY_KEY_MINUS, 0, ACT_SPLIT_ROWS);
  bind_add(c, GHOSTTY_KEY_X, 0, ACT_CLOSE_PANE);
  /* `?` twice, because whether it arrives with shift depends on the outer
   * terminal: as a plain byte there is no modifier to be had, and under the
   * kitty keyboard protocol (which the client asks for) there is. Binding one
   * of them is a binding that works on the author's machine. */
  bind_add(c, GHOSTTY_KEY_SLASH, MOD_SHIFT, ACT_HELP);
  bind_add(c, GHOSTTY_KEY_SLASH, 0, ACT_HELP);
  bind_add(c, GHOSTTY_KEY_R, 0, ACT_RERUN);
  bind_add(c, GHOSTTY_KEY_Z, 0, ACT_ZOOM);
  bind_add(c, GHOSTTY_KEY_M, 0, ACT_MINIMIZE);
  bind_add(c, GHOSTTY_KEY_H, 0, ACT_FOCUS_LEFT);
  bind_add(c, GHOSTTY_KEY_L, 0, ACT_FOCUS_RIGHT);
  bind_add(c, GHOSTTY_KEY_K, 0, ACT_FOCUS_UP);
  bind_add(c, GHOSTTY_KEY_J, 0, ACT_FOCUS_DOWN);
  bind_add(c, GHOSTTY_KEY_ARROW_LEFT, 0, ACT_FOCUS_LEFT);
  bind_add(c, GHOSTTY_KEY_ARROW_RIGHT, 0, ACT_FOCUS_RIGHT);
  bind_add(c, GHOSTTY_KEY_ARROW_UP, 0, ACT_FOCUS_UP);
  bind_add(c, GHOSTTY_KEY_ARROW_DOWN, 0, ACT_FOCUS_DOWN);
  bind_add(c, GHOSTTY_KEY_O, 0, ACT_FOCUS_NEXT);
  bind_add(c, GHOSTTY_KEY_H, MOD_SHIFT, ACT_RESIZE_LEFT);
  bind_add(c, GHOSTTY_KEY_L, MOD_SHIFT, ACT_RESIZE_RIGHT);
  bind_add(c, GHOSTTY_KEY_K, MOD_SHIFT, ACT_RESIZE_UP);
  bind_add(c, GHOSTTY_KEY_J, MOD_SHIFT, ACT_RESIZE_DOWN);
  bind_add(c, GHOSTTY_KEY_ARROW_LEFT, MOD_SHIFT, ACT_RESIZE_LEFT);
  bind_add(c, GHOSTTY_KEY_ARROW_RIGHT, MOD_SHIFT, ACT_RESIZE_RIGHT);
  bind_add(c, GHOSTTY_KEY_ARROW_UP, MOD_SHIFT, ACT_RESIZE_UP);
  bind_add(c, GHOSTTY_KEY_ARROW_DOWN, MOD_SHIFT, ACT_RESIZE_DOWN);
  bind_add(c, GHOSTTY_KEY_C, 0, ACT_NEW_TAB);
  bind_add(c, GHOSTTY_KEY_N, 0, ACT_NEXT_TAB);
  bind_add(c, GHOSTTY_KEY_P, 0, ACT_PREV_TAB);
  bind_add(c, GHOSTTY_KEY_F, 0, ACT_FINDER);
  bind_add(c, GHOSTTY_KEY_PAGE_UP, 0, ACT_SCROLL_PAGE_UP);
  bind_add(c, GHOSTTY_KEY_PAGE_DOWN, 0, ACT_SCROLL_PAGE_DOWN);
  bind_add(c, GHOSTTY_KEY_HOME, 0, ACT_SCROLL_TOP);
  bind_add(c, GHOSTTY_KEY_END, 0, ACT_SCROLL_BOTTOM);
  bind_add(c, GHOSTTY_KEY_D, 0, ACT_DETACH);
  bind_add(c, GHOSTTY_KEY_Q, 0, ACT_QUIT);
  for (int i = 0; i < 9; i++)
    bind_add(c, GHOSTTY_KEY_DIGIT_1 + i, 0, (action_t)(ACT_SELECT_TAB_1 + i));
}

void config_free(config_t *c) {
  for (size_t i = 0; i < c->nexprs; i++) expr_free(c->exprs[i]);
  free(c->exprs);
  free(c->binds);
  free(c->shell);
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

    {ACT_HELP, "session", "this list"},
    {ACT_DETACH, "session", "detach, leave it running"},
    {ACT_QUIT, "session", "quit the session"},
    {ACT_LITERAL_PREFIX, "session", "send the prefix itself"},
};

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

action_t config_lookup(const config_t *c, int key, uint16_t mods) {
  /* caps/num lock must not make a binding stop working */
  mods &= (uint16_t)(MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_SUPER);
  for (size_t i = 0; i < c->nbinds; i++)
    if (c->binds[i].key == key && c->binds[i].mods == mods)
      return c->binds[i].action;
  return ACT_NONE;
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
  c->pane_buttons =
      kdl_arg_bool(kdl_child(root, "pane_buttons"), 0, c->pane_buttons);
  const char *zm = kdl_arg(kdl_child(root, "zoom_mark"), 0, NULL);
  if (zm) snprintf(c->zoom_mark, sizeof c->zoom_mark, "%s", zm);
  const char *zo = kdl_arg(kdl_child(root, "zoom_on_mark"), 0, NULL);
  if (zo) snprintf(c->zoom_on_mark, sizeof c->zoom_on_mark, "%s", zo);
  const char *cm = kdl_arg(kdl_child(root, "close_mark"), 0, NULL);
  if (cm) snprintf(c->close_mark, sizeof c->close_mark, "%s", cm);
  const char *mm = kdl_arg(kdl_child(root, "min_mark"), 0, NULL);
  if (mm) snprintf(c->min_mark, sizeof c->min_mark, "%s", mm);
  const char *nt = kdl_arg(kdl_child(root, "newtab_mark"), 0, NULL);
  if (nt) snprintf(c->newtab_mark, sizeof c->newtab_mark, "%s", nt);
  c->keep_dead = kdl_arg_bool(kdl_child(root, "keep_dead"), 0, c->keep_dead);
  c->bell_indicator =
      kdl_arg_bool(kdl_child(root, "bell_indicator"), 0, c->bell_indicator);
  const char *bm = kdl_arg(kdl_child(root, "bell_mark"), 0, NULL);
  if (bm) snprintf(c->bell_mark, sizeof c->bell_mark, "%s", bm);

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
  {
    long v = kdl_arg_int(kdl_child(root, "modal_scrim"), 0, c->modal_scrim);
    c->modal_scrim = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
  }


  const char *sh = kdl_arg(kdl_child(root, "shell"), 0, NULL);
  if (sh) {
    free(c->shell);
    c->shell = strdup(sh);
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
  if (shaders) c->nshaders = parse_shader_list(c, shaders, c->shaders, err, errcap);

  /* `states { drop_target { grayscale amount=200; dim amount=140 } }` — what a
   * pane looks like while it is in a state. Naming a state at all replaces its
   * default outright, including with nothing, which is how you turn one off. */
  const kdl_node_t *states = kdl_child(root, "states");
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
      c->state_n[st] = parse_shader_list(c, k, c->state_shaders[st], err, errcap);
    }
  }

  const kdl_node_t *theme = kdl_child(root, "theme");
  if (theme) {
    struct {
      const char *name;
      color_t *slot;
    } colors[] = {
        {"default_fg", &c->default_fg},
        {"default_bg", &c->default_bg},
        {"frame_focus", &c->frame_focus},
        {"frame_idle", &c->frame_idle},
        {"title", &c->title_focus},
        {"title_idle", &c->title_idle},
        {"button_fg", &c->button_fg},
        {"button_bg", &c->button_bg},
        {"button_bg_idle", &c->button_bg_idle},
        {"guide", &c->guide},
        {"resize", &c->resize},
        {"drop_target", &c->drop_target},
        {"scroll_fg", &c->scroll_fg},
        {"scroll_bg", &c->scroll_bg},
        {"header", &c->header},
        {"header_hover", &c->header_hover},
        {"header_hover_title", &c->header_hover_title},
        {"tab_active_fg", &c->tab_active_fg},
        {"tab_active_bg", &c->tab_active_bg},
        {"tab_active_hover_fg", &c->tab_active_hover_fg},
        {"tab_idle", &c->tab_idle},
        {"tab_hover", &c->tab_hover},
        {"prefix_fg", &c->prefix_fg},
        {"prefix_bg", &c->prefix_bg},
        {"tab_count", &c->tab_count},
        {"status", &c->status},
        {"status_state", &c->status_state},
        {"finder_fg", &c->finder_fg},
        {"finder_bg", &c->finder_bg},
        {"finder_sel_fg", &c->finder_sel_fg},
        {"finder_sel_bg", &c->finder_sel_bg},
        {"bell", &c->bell},
        {"modal_fg", &c->modal_fg},
        {"modal_bg", &c->modal_bg},
        {"modal_border", &c->modal_border},
        {"modal_title", &c->modal_title},
        {"modal_button", &c->modal_button},
        {"modal_button_hover", &c->modal_button_hover},
        {"dead", &c->dead},
        {"hint", &c->hint},
        {"minbar", &c->minbar},
        {"minbar_hover", &c->minbar_hover},
        {"pane_button", &c->pane_button},
        {"pane_button_hover", &c->pane_button_hover},
        {"rename_fg", &c->rename_fg},
        {"rename_bg", &c->rename_bg},
        {"toast_fg", &c->toast_fg},
        {"toast_bg", &c->toast_bg},
    };
    for (size_t i = 0; i < sizeof colors / sizeof *colors; i++) {
      const char *v = kdl_arg(kdl_child(theme, colors[i].name), 0, NULL);
      if (v && !parse_color(v, colors[i].slot) && err && !err[0])
        snprintf(err, errcap, "bad colour for %s: %s", colors[i].name, v);
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
    for (size_t i = 0; i < keys->nkids; i++) {
      const kdl_node_t *b = keys->kids[i];
      if (strcmp(b->name, "bind") != 0) continue;
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
      bind_add(c, k, m, a);
    }
  }

  kdl_free(root);
  return true;
}
