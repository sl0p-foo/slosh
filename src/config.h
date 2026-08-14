/* The opinionated defaults, and the file that overrides them.
 *
 * Everything here has a working value compiled in, so a missing or broken
 * config file costs you a warning, never a terminal (the fail-open property
 * D9 asks of the CLI, applied to the mux itself).
 */
#ifndef SL0PPTY_CONFIG_H
#define SL0PPTY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shader.h"
#include "sl0ppty.h"

/* Pane states a shader can be hung off, in the order they are tested. Only
 * states a pane can actually be seen in: a pane whose program exits is reaped
 * before the next paint, so there is deliberately no "dead" here — it would
 * name a shader that could never draw a frame. */
typedef enum {
  PSTATE_DRAGGING,    /* the pane you have hold of */
  PSTATE_DROP_HOVER,  /* the one under the pointer, where it would land */
  PSTATE_DROP_TARGET, /* the others, all of them somewhere it could go */
  PSTATE_SUSPENDED,   /* laid out, never started */
  PSTATE_SCROLLED,    /* looking at scrollback rather than the present */
  PSTATE_UNFOCUSED,
  PSTATE_COUNT,
} pane_state_t;

/* The config name for a state, e.g. "drop_target". */
const char *pane_state_name(pane_state_t s);

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
  uint16_t toast_ms;
  uint16_t hover_delay_ms; /* how long the pointer must rest to arm a guide */ /* how long an announcement stays up */ /* rows per wheel notch */
  uint16_t double_click_ms; /* how close two clicks must be to be a double */
  /* Shader strengths, 0..255, 0 being off. Ambient dimming is a taste and is
   * off by default; the drag greying is transient and on. */
  /* Colour passes every pane gets, in the order they were written. Ordinary
   * shaders rather than policy: the session has no opinion about these, you
   * asked for them. */
  shader_t shaders[SHADE_MAX];
  size_t nshaders;

  /* What a pane looks like when it is in a particular state. A pane is
   * usually in several at once — unfocused *and* scrolled, suspended *and*
   * something you could drop onto — so exactly one wins, the first that
   * matches in the order below. That order is fixed rather than taken from
   * the config, because it is a ranking by urgency and not a preference: a
   * pane you are holding should not be recoloured by anything, and a mode the
   * whole screen is in outranks an ambient hint about one pane.
   *
   * Stacking them instead would let two reasons to be grey compound into one
   * muddy grey that reads as neither, which is the mistake the drag policy
   * already had to avoid by hand. */
  shader_t state_shaders[PSTATE_COUNT][SHADE_MAX];
  size_t state_n[PSTATE_COUNT];
  bool status_bar;
  bool focus_follows_mouse;

  /* What a cell's "terminal default" colour resolves to when a shader has to
   * compute on it. We cannot know the client's real default, and most text is
   * default-coloured, so a shader would otherwise leave nearly everything
   * alone. Only consulted while shading: an unshaded pane still defers to the
   * terminal exactly as before. */
  color_t default_fg, default_bg;

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
/* $SL0PPTY_CONFIG, else $XDG_CONFIG_HOME/sl0ppty/config.kdl, else ~/.config/… */
const char *config_default_path(void);
void config_free(config_t *c);

action_t config_lookup(const config_t *c, int key, uint16_t mods);
/* "ctrl+a", "alt+left", "\\", "f" -> key + mods. False if unparseable. */
bool config_parse_chord(const char *text, int *out_key, uint16_t *out_mods);

#endif /* SL0PPTY_CONFIG_H */
