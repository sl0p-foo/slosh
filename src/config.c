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
    {"close-pane", ACT_CLOSE_PANE},   {"focus-left", ACT_FOCUS_LEFT},
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
  c->min_pane_cols = 24;
  c->min_pane_rows = 6;
  c->scroll_lines = 3;
  c->toast_ms = 2500;
  c->hover_delay_ms = 250;
  c->double_click_ms = 400;
  c->dim_unfocused = 0;
  c->drag_grayscale = 200;
  c->drag_dim = 140;
  c->status_bar = true;
  c->focus_follows_mouse = true;

  c->default_fg = rgb(0xff, 0xff, 0xff);
  c->default_bg = rgb(0x00, 0x00, 0x00);

  c->frame_focus = rgb(0xff, 0x5f, 0xd7);
  c->frame_idle = rgb(0x45, 0x45, 0x4a);
  c->title_focus = rgb(0xff, 0xff, 0xff);
  c->button_fg = rgb(0x14, 0x14, 0x18);
  c->button_bg = rgb(0xff, 0x5f, 0xd7);
  c->button_bg_idle = rgb(0x55, 0x55, 0x5c);

  c->prefix_key = GHOSTTY_KEY_A;
  c->prefix_mods = MOD_CTRL;

  bind_add(c, GHOSTTY_KEY_BACKSLASH, 0, ACT_SPLIT_COLS);
  bind_add(c, GHOSTTY_KEY_MINUS, 0, ACT_SPLIT_ROWS);
  bind_add(c, GHOSTTY_KEY_X, 0, ACT_CLOSE_PANE);
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
  free(c->binds);
  free(c->shell);
  memset(c, 0, sizeof *c);
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
  c->focus_follows_mouse = kdl_arg_bool(kdl_child(root, "focus_follows_mouse"), 0,
                                        c->focus_follows_mouse);

  const char *align = kdl_arg(kdl_child(root, "title_align"), 0, NULL);
  if (align) {
    if (strcmp(align, "left") == 0) c->title_align = ALIGN_LEFT;
    else if (strcmp(align, "right") == 0) c->title_align = ALIGN_RIGHT;
    else c->title_align = ALIGN_CENTER;
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
  c->dim_unfocused = (uint8_t)kdl_arg_int(kdl_child(root, "dim_unfocused"), 0,
                                          c->dim_unfocused);
  c->drag_grayscale = (uint8_t)kdl_arg_int(kdl_child(root, "drag_grayscale"), 0,
                                           c->drag_grayscale);
  c->drag_dim =
      (uint8_t)kdl_arg_int(kdl_child(root, "drag_dim"), 0, c->drag_dim);

  const char *sh = kdl_arg(kdl_child(root, "shell"), 0, NULL);
  if (sh) {
    free(c->shell);
    c->shell = strdup(sh);
  }

  const kdl_node_t *theme = kdl_child(root, "theme");
  if (theme) {
    struct {
      const char *name;
      color_t *slot;
    } colors[] = {
        {"default_fg", &c->default_fg},   {"default_bg", &c->default_bg},
        {"frame_focus", &c->frame_focus}, {"frame_idle", &c->frame_idle},
        {"title", &c->title_focus},       {"button_fg", &c->button_fg},
        {"button_bg", &c->button_bg},     {"button_bg_idle", &c->button_bg_idle},
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
