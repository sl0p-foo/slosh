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

#include "expr.h"
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
  ACT_HELP,
  ACT_EDIT_CONFIG,
  ACT_LITERAL_PREFIX,
  ACT_SELECT_TAB_1, /* .. +8, so digits stay one entry each */
} action_t;

typedef struct {
  int key;       /* GhosttyKey */
  uint16_t mods; /* MOD_* */
  action_t action;
  /* Fires without the leader, taking that chord away from every program in
   * every pane. Deliberately possible and deliberately opt-in: it is the
   * user's terminal and their keyboard, and a multiplexer that refuses to get
   * out of the way is its own kind of wrong. */
  bool direct;
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
  /* What sl0ppty this is, in the middle of the status line, whenever there is
   * no hint to put there. The slot is otherwise empty most of the time, and
   * "which build am I attached to" is the first question when a session is
   * behaving oddly -- a session keeps the binary it started with, so the
   * answer is not whatever was built last. */
  bool version_banner;
  bool pane_buttons;
  /* Marks of similar visual weight, so the even spacing between the buttons
   * reads as even: a glyph that floats in a mostly-empty cell donates its own
   * whitespace to the gap beside it and looks further away than its
   * neighbours, which is what an en dash did here. */
  char zoom_mark[16], zoom_on_mark[16], close_mark[16], min_mark[16];
  /* The new-tab button at the end of the strip. Drawn with a space each side,
   * so a one-character mark is still a three-cell target. */
  char newtab_mark[16];
  bool bell_indicator;
  /* What to draw. A short string rather than a fixed glyph: the obvious
   * choice is an emoji bell, and emoji are drawn two columns wide by many
   * terminals while chrome here is booked as one — so the default is narrow
   * and anyone who knows their terminal can say otherwise. */
  char bell_mark[16];
  /* Which dead panes stay, showing what they printed and offering to run
   * again, and which just go.
   *
   * The line is whether the pane was given a *command*. A pane told to run
   * `npm run dev` is part of the session's shape and its death is news: you
   * want the error, and you want the button that runs it again. A pane that
   * is a shell -- one you split off to do something in -- is finished when
   * you type `exit`, and leaving its corpse on screen to be dismissed is
   * exactly the fussiness a terminal should not have.
   *
   * Not *who made it*: a shell is a shell whether it came from a layout file
   * or from C-a \. That also means a session restored from dump-layout
   * behaves like the one it was dumped from, since the dump writes `command=`
   * only for panes that had one. */
  enum { KEEP_DEAD_NONE, KEEP_DEAD_COMMANDS, KEEP_DEAD_ALL } keep_dead;
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
  /* How far everything behind a modal is pushed back, 0..255, 0 being off.
   * A modal that floats over a screen as bright as it is reads as another
   * pane rather than as something in front. */
  uint8_t modal_scrim;
  /* How far the panes you are *not* in are pushed back, 0..255 and 0 for not
   * at all. A knob rather than only a `states { unfocused { } }` chain,
   * because "dim the other panes" is a thing people want by name and should
   * not have to know the shader vocabulary to ask for -- and because the
   * argument against shipping it on was that turning it off should be
   * obvious, which a single number is and a chain is not.
   *
   * Writing the state chain by hand still wins: the knob only fills it in
   * when the config has not. */
  uint8_t dim_unfocused;

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

  /* Modals: a surface that floats over the layout, with its own frame. Named
   * separately from the pane colours it resembles because it is a different
   * *surface* — it sits on top of everything, it is opaque, and it has to
   * stay legible against a background that has been dimmed underneath it.
   * Borrowing the pane button colour here is what made the close button
   * invisible: dim on dim. */
  color_t modal_fg, modal_bg;
  color_t modal_border, modal_title;
  color_t modal_button, modal_button_hover;

  /* announcements */
  color_t toast_fg, toast_bg;

  /* keys */
  int prefix_key;
  uint16_t prefix_mods;
  binding_t *binds;
  size_t nbinds;

  /* Every amount expression this config compiled. A shader_t is copied by
   * value all over the place and several copies can point at one program, so
   * ownership lives here — with the config that parsed them, freed when it is
   * replaced. */
  expr_prog_t **exprs;
  size_t nexprs;

  /* what a new pane runs; NULL means $SHELL */
  char *shell;
  /* What `edit-config` opens the file with; NULL means $EDITOR, then vi. */
  char *editor;

  /* Where to look for shader plugins (`*.so`). NULL means the default: a
   * `shaders` directory beside the config file. Read before the `shaders` and
   * `states` blocks are parsed, because what they are allowed to name depends
   * on what has been loaded. */
  char *shader_dir;
} config_t;

/* Defaults, then <config dir>/config.kdl on top. Never fails: on a bad file
 * it keeps the defaults and writes the reason to `err`. */
void config_defaults(config_t *c);
bool config_load(config_t *c, const char *path, char *err, size_t errcap);
/* $SL0PPTY_CONFIG, else $XDG_CONFIG_HOME/sl0ppty/config.kdl, else ~/.config/… */
const char *config_default_path(void);
void config_free(config_t *c);

/* What a chord does after the leader. Direct bindings answer here too, so
 * pressing the leader first never makes a binding stop working. */
action_t config_lookup(const config_t *c, int key, uint16_t mods);
/* What a chord does on its own, with no leader pressed. ACT_NONE for the
 * overwhelming majority of keys, which is what lets them reach the pane. */
action_t config_lookup_direct(const config_t *c, int key, uint16_t mods);
/* Whether any binding fires without the leader, so the cheatsheet knows
 * whether it has a section to draw. */
bool config_has_direct(const config_t *c);
/* The reverse of config_parse_chord: what to call a binding on screen. A
 * shifted letter is written the way you would type it ("H"), because "S-h" is
 * a description of a keystroke rather than a keystroke. */
void config_chord_name(int key, uint16_t mods, char *out, size_t cap);
/* A phrase for the cheatsheet, and which group it belongs under. `group` is
 * one of a fixed set, in the order they should be shown. */
const char *config_action_label(action_t a);
const char *config_action_group(action_t a);
/* "ctrl+a", "alt+left", "\\", "f" -> key + mods. False if unparseable. */
bool config_parse_chord(const char *text, int *out_key, uint16_t *out_mods);

#endif /* SL0PPTY_CONFIG_H */
