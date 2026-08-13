/* The opinionated defaults, and the file that overrides them.
 *
 * Everything here has a working value compiled in, so a missing or broken
 * config file costs you a warning, never a terminal (the fail-open property
 * D9 asks of the CLI, applied to the mux itself).
 */
#ifndef SL0PTTY_CONFIG_H
#define SL0PTTY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sl0ptty.h"

typedef enum {
  ACT_NONE = 0,
  ACT_SPLIT_COLS,
  ACT_SPLIT_ROWS,
  ACT_CLOSE_PANE,
  ACT_FOCUS_LEFT,
  ACT_FOCUS_RIGHT,
  ACT_FOCUS_UP,
  ACT_FOCUS_DOWN,
  ACT_FOCUS_NEXT,
  ACT_NEW_TAB,
  ACT_NEXT_TAB,
  ACT_PREV_TAB,
  ACT_FINDER,
  ACT_SCROLL_UP,
  ACT_SCROLL_DOWN,
  ACT_SCROLL_PAGE_UP,
  ACT_SCROLL_PAGE_DOWN,
  ACT_SCROLL_TOP,
  ACT_SCROLL_BOTTOM,
  ACT_RESIZE_LEFT,
  ACT_RESIZE_RIGHT,
  ACT_RESIZE_UP,
  ACT_RESIZE_DOWN,
  ACT_DETACH,
  ACT_QUIT,
  ACT_LITERAL_PREFIX,
  ACT_SELECT_TAB_1, /* .. +8, so digits stay one entry each */
} action_t;

typedef struct {
  int key;       /* GhosttyKey */
  uint16_t mods; /* MOD_* */
  action_t action;
} binding_t;

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } align_t;

typedef struct {
  /* geometry */
  uint16_t gap, gap_aspect, pad;
  bool rounded;
  align_t title_align;
  uint16_t min_pane_cols, min_pane_rows;
  uint16_t scroll_lines;
  uint16_t toast_ms; /* how long an announcement stays up */ /* rows per wheel notch */
  bool status_bar;
  bool focus_follows_mouse;

  /* theme */
  color_t frame_focus, frame_idle, title_focus;
  color_t button_fg, button_bg, button_bg_idle;

  /* keys */
  int prefix_key;
  uint16_t prefix_mods;
  binding_t *binds;
  size_t nbinds;

  /* what a new pane runs; NULL means $SHELL */
  char *shell;
} config_t;

/* Defaults, then <config dir>/config.kdl on top. Never fails: on a bad file
 * it keeps the defaults and writes the reason to `err`. */
void config_defaults(config_t *c);
bool config_load(config_t *c, const char *path, char *err, size_t errcap);
/* $SL0PTTY_CONFIG, else $XDG_CONFIG_HOME/sl0ptty/config.kdl, else ~/.config/… */
const char *config_default_path(void);
void config_free(config_t *c);

action_t config_lookup(const config_t *c, int key, uint16_t mods);
/* "ctrl+a", "alt+left", "\\", "f" -> key + mods. False if unparseable. */
bool config_parse_chord(const char *text, int *out_key, uint16_t *out_mods);

#endif /* SL0PTTY_CONFIG_H */
