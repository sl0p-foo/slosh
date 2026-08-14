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
 * states a pane can actually be seen in — which `dead` now is: a pane whose
 * program exits is kept until it is dismissed, so there is a frame to draw.
 * It ranks just under the drag states, above every ambient one: nothing about
 * a pane is more worth knowing than that it is over. */
typedef enum {
  PSTATE_DRAGGING,    /* the pane you have hold of */
  PSTATE_DROP_HOVER,  /* the one under the pointer, where it would land */
  PSTATE_DROP_TARGET, /* the others, all of them somewhere it could go */
  PSTATE_DEAD,        /* its program exited; waiting to be re-run or closed */
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
  ACT_RERUN,
  ACT_ZOOM,
  ACT_MINIMIZE,
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
  /* Cells between a frame's corner and the start of its title. The title
   * always carries a space on each side on top of this, so a title can never
   * end up welded to the rule beside it. */
  uint16_t title_inset;
  /* Mark a pane's titlebar when a BEL arrives there and nobody has looked
   * since. Off makes a bell silent *and* invisible, which is a real choice. */
  /* Buttons in the top-right of a pane's frame: zoom, and close. Off leaves
   * the keyboard bindings alone; it is about the affordance, not the verb. */
  /* A word in the middle of the status line for whatever the pointer is on.
   * Discoverability for a frame whose affordances are mostly one character
   * wide; off if you already know them. */
  bool hints;
  bool pane_buttons;
  /* Marks of similar visual weight, so the even spacing between the buttons
   * reads as even: a glyph that floats in a mostly-empty cell donates its own
   * whitespace to the gap beside it and looks further away than its
   * neighbours, which is what an en dash did here. */
  char zoom_mark[16], zoom_on_mark[16], close_mark[16], min_mark[16];
  bool bell_indicator;
  /* What to draw. A short string rather than a fixed glyph: the obvious
   * choice is an emoji bell, and emoji are drawn two columns wide by many
   * terminals while chrome here is booked as one — so the default is narrow
   * and anyone who knows their terminal can say otherwise. */
  char bell_mark[16];
  /* A pane whose program exits stays, showing what it printed and offering to
   * run it again, until it is closed. Off restores the old behaviour: the
   * pane vanishes on the next paint, and with the last pane so does the
   * session. That is a real choice — a one-shot layout wants it — but it is
   * not the default, because a pane that disappears takes its own error
   * message with it. */
  bool keep_dead;
  uint16_t min_pane_cols, min_pane_rows;
  /* The smallest pane a split is allowed to *produce*. min_pane is the point
   * below which the layout gives up and collapses a pane; this is the point
   * below which splitting stopped being worth offering, which is a larger
   * number and a different question. Never used below min_pane: refusing to
   * offer something the layout would collapse anyway is the floor. */
  uint16_t min_split_cols, min_split_rows;
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
  bool status_bar;  /* the strip along the top: tabs, prefix, pane count */
  bool status_line; /* the line along the bottom: what you are looking at */
  /* How far the strip and the line are held off the left and right edges.
   * Separate from `gap`, which is the space around the *panes*: the two happen
   * to match by default and there is no reason they must. */
  uint16_t status_pad;
  bool focus_follows_mouse;

  /* What a cell's "terminal default" colour resolves to when a shader has to
   * compute on it. We cannot know the client's real default, and most text is
   * default-coloured, so a shader would otherwise leave nearly everything
   * alone. Only consulted while shading: an unshaded pane still defers to the
   * terminal exactly as before. */
  color_t default_fg, default_bg;

  /* theme
   *
   * One name per decision. These began as six names doing thirty jobs, which
   * meant the split guide could not be recoloured without also recolouring the
   * focused frame, because they were the same entry. Several still share a
   * default — that is a statement about what looks right together, not about
   * them being the same thing. */

  /* panes */
  color_t frame_focus, frame_idle;
  color_t title_focus, title_idle;

  /* things you can press: the pane's own OSC 5577 buttons */
  color_t button_fg, button_bg, button_bg_idle;

  /* affordances that appear under the pointer */
  color_t guide;       /* the split guide: armed edge and dashed boundary */
  color_t resize;      /* the handle in the gap between two panes */
  color_t drop_target; /* the frame of a pane a drag could land on */

  /* the scrollback indicator in a pane's frame */
  color_t scroll_fg, scroll_bg;

  /* rows of a tab that has collapsed into a list */
  color_t header, header_hover, header_hover_title;

  /* the strip along the top */
  color_t tab_active_fg, tab_active_bg;
  /* The active tab keeps its fill while the pointer is on it -- it is still
   * the tab you are in -- so its hover shows in the text instead. */
  color_t tab_active_hover_fg;
  color_t tab_idle, tab_hover;
  color_t prefix_fg, prefix_bg; /* the "C-a" badge */
  color_t tab_count;            /* how many panes in the session */

  /* the line along the bottom */
  color_t status, status_state;

  /* the pane finder */
  color_t finder_fg, finder_bg, finder_sel_fg, finder_sel_bg;

  /* the in-place editor a double-click opens on a pane's title */
  color_t rename_fg, rename_bg;

  /* the bell mark in a pane's titlebar */
  color_t bell;

  /* what a dead pane says: the line in its backlog, and the word in the
   * status line and its own frame */
  color_t dead;

  /* the frame's own buttons */
  color_t pane_button, pane_button_hover;

  /* the row of minimised panes along the bottom */
  color_t minbar, minbar_hover;

  /* the hint in the middle of the status line */
  color_t hint;

  /* announcements */
  color_t toast_fg, toast_bg;

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
