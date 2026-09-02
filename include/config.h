/* The opinionated defaults, and the file that overrides them.
 *
 * Everything here has a working value compiled in, so a missing or broken
 * config file costs you a warning, never a terminal (the fail-open property
 * D9 asks of the CLI, applied to the mux itself).
 */
#ifndef SLOSH_CONFIG_H
#define SLOSH_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "expr.h"
#include "shader.h"
#include "slosh.h"

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
  /* A BEL arrived and nobody has looked since. Above the ambient states
   * because it is *news* — the whole point of a bell is to be noticed on a
   * pane you were not watching — and below `dead`, because a pane that rang
   * and then exited is a pane that exited. It ends when you look at the pane,
   * which is what answering a bell is. */
  PSTATE_BELL,
  PSTATE_SCROLLED, /* looking at scrollback rather than the present */
  /* Lifted out of the layout, drawn on top. Above `unfocused`, so a float is
   * never dimmed by dim_unfocused: full strength is what keeps the thing on
   * top reading as lifted off the page — the same reason `dragging` keeps no
   * default. Ships no chain of its own either; the shadow does the telling
   * apart, and a config that wants colour writes `states { floating { } }`. */
  PSTATE_FLOATING,
  PSTATE_UNFOCUSED,
  PSTATE_COUNT,
} pane_state_t;

/* The config name for a state, e.g. "drop_target". */
const char *pane_state_name(pane_state_t s);

typedef enum {
  ACT_NONE = 0,
  ACT_SPLIT,
  ACT_SPLIT_COLS,
  ACT_SPLIT_ROWS,
  ACT_CLOSE_PANE,
  ACT_RERUN,
  ACT_ZOOM,
  ACT_MINIMIZE,
  ACT_FLOAT,
  ACT_NEW_FLOAT,
  ACT_FLOAT_GROW,
  ACT_FLOAT_SHRINK,
  ACT_SET_PURPOSE,
  ACT_RENAME_PANE,
  ACT_RENAME_TAB,
  ACT_ROTATE_LAYOUT,
  ACT_FOCUS_LEFT,
  ACT_FOCUS_RIGHT,
  ACT_FOCUS_UP,
  ACT_FOCUS_DOWN,
  ACT_FOCUS_NEXT,
  ACT_NEW_TAB,
  ACT_CLOSE_TAB,
  ACT_NEXT_TAB,
  ACT_PREV_TAB,
  ACT_FINDER,
  ACT_PALETTE,
  ACT_WORKSPACES,
  ACT_SAVE_WORKSPACE,
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
  ACT_EQUALIZE,
  ACT_CLEAR_SHADERS,
  ACT_PANE_TO_NEXT_TAB,
  ACT_PANE_TO_PREV_TAB,
  ACT_PANE_TO_NEW_TAB,
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

/* How many files one config may be built from: itself plus its includes. Small
 * and fixed, because a config assembled from more than this many pieces is a
 * thing nobody can read. */
#define CONFIG_FILES_MAX 16

/* How many project roots one config may name. `~/dev` and `~/work` is the case;
 * a person with more than this many places they keep code has a search problem
 * rather than a list problem. */
#define PROJECT_ROOTS_MAX 8

/* How many complaints one load keeps. A config with more than this many
 * problems in it has one problem, and the list is long enough to say so. */
#define CONFIG_MSGS_MAX 32

typedef struct {
  /* geometry */
  uint16_t gap, gap_aspect;
  /* Space between a pane's frame and its contents, per side, in the same unit
   * `gap` uses: ROWS. A horizontal one is multiplied by `gap_aspect` on the way
   * out, so `padding 1` is a square-looking ring rather than a squashed one —
   * and so a number means the same thing wherever it appears, however many of
   * them were written. Written as 1, 2 or 4 values, CSS order. */
  uint16_t pad_top, pad_right, pad_bottom, pad_left;
  /* Shared borders instead of gaps: panes pack flush against 1-cell divider
   * lines, one outer frame rings the tab, and each pane's title rides the
   * line above it. `gap` stops applying — the boundary *is* the divider —
   * while `padding` still works and `gap_aspect` keeps its other jobs
   * (square-looking padding, the float nudge step). The dividers carry the
   * same resize drag the gaps did; interior edges give up their
   * click-to-split — a one-cell line cannot be both verbs — and the outer
   * frame keeps it. */
  bool compact;
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
  /* What slosh this is, in the middle of the status line, whenever there is
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
  /* 32 bytes because a mark is a grapheme cluster, not a character: a
   family emoji is eighteen bytes on its own. Truncating one mid-
   codepoint used to put invalid UTF-8 on the wire. */
  char zoom_mark[32], zoom_on_mark[32], close_mark[32], min_mark[32];
  /* The new-tab button at the end of the strip. Drawn with a space each side,
   * so a one-character mark is still a three-cell target. */
  char newtab_mark[32];
  bool bell_indicator;
  /* What to draw. A short string rather than a fixed glyph: the obvious
   * choice is an emoji bell, and emoji are drawn two columns wide by many
   * terminals while chrome here is booked as one — so the default is narrow
   * and anyone who knows their terminal can say otherwise. */
  char bell_mark[32];
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
  uint16_t scroll_lines; /* rows per wheel notch */
  /* How much history a pane keeps. `scrollback` is the number people mean --
   * lines -- and `scrollback_bytes` is the ceiling that keeps one very wide,
   * heavily styled pane from spending the machine to honour it: lib-vt applies
   * whichever limit is reached first, and both depend on things a line count
   * cannot see. 0 lines is no scrollback; 0 bytes is no ceiling.
   *
   * Not a `uint16_t`, because 65,535 is a number somebody will want to exceed
   * and a silently wrapped limit is worse than a refused one. */
  size_t scrollback;
  size_t scrollback_bytes;
  uint16_t toast_ms; /* how long an announcement stays up */
  /* How long the logo splash stays over a freshly attached screen, 0 for
   * never. Attach-only: a headless or scripted session has nobody to greet. */
  uint16_t splash_ms;
  uint16_t hover_delay_ms; /* how long the pointer must rest to arm a guide */
  /* How often a selection drag held past a pane's top or bottom edge scrolls
   * another step. The step size is how far past the edge the pointer is, so
   * this is the metronome and the hand is the throttle. */
  uint16_t select_scroll_ms;
  uint16_t double_click_ms; /* how close two clicks must be to be a double */
  /* What a double-click's word stops at, listed as *separators* rather than as
   * word characters: anything unlisted belongs to a word, so text nobody
   * enumerated -- CJK, accents, emoji -- needs no table. Whitespace always
   * separates, listed or not. */
  char word_separators[64];
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
  /* How dark the cell of shade a floating pane casts on what it covers,
   * 0..255 and 0 for no shadow. The shadow is what tells a float from a tile
   * at a glance — the frame is the same frame — which is why it ships on and
   * why it is a number rather than a state chain: turning it off should not
   * require knowing the states block exists (the dim_unfocused argument). */
  uint8_t float_shadow;

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

  /* Whether any file in this load named `scrolled` (or `bell`) itself. The
   * default wash over scrollback and the default bell flash are derived from
   * the theme *after* each file's theme block -- scroll_bg and bell
   * respectively -- so a theme moves them with their indicators. But a chain
   * somebody wrote, in this file or in one it included, must stand. A local
   * flag cannot say that across an include chain, so they live here and are
   * cleared with the rest of the defaults. */
  bool scrolled_declared;
  bool bell_declared;

  /* The same two things again, over a pane's *frame* rather than its contents:
   * any entry in `shaders` or in a state's block carrying `where="chrome"`
   * lands here instead. Separate chains rather than a flag on each shader,
   * because a pass is per rect: the contents and the frame are two rects and
   * therefore two passes, and a chain is exactly the list one pass runs. It
   * also keeps the flag out of the plugin ABI, which has no business knowing
   * where a session chose to run an effect. */
  shader_t chrome_shaders[SHADE_MAX];
  size_t nchrome_shaders;
  shader_t chrome_state_shaders[PSTATE_COUNT][SHADE_MAX];
  size_t chrome_state_n[PSTATE_COUNT];
  /* How often to repaint while something on screen is animated -- which means
   * a shader whose amount reads the clock. Nothing else needs a frame clock:
   * every other repaint has an event behind it. 0 turns animation off, in the
   * sense that an animated shader then only advances when something else
   * causes a frame. */
  uint16_t anim_ms;
  bool status_bar;  /* the strip along the top: tabs, prefix, pane count */
  bool status_line; /* the line along the bottom: what you are looking at */
  /* How far the strip and the line are held off the left and right edges.
   * Separate from `gap`, which is the space around the *panes*: the two happen
   * to match by default and there is no reason they must. */
  uint16_t status_pad;
  bool focus_follows_mouse;

  /* Whether Ctrl-D at an idle shell prompt exits that shell, the way the tty
   * line discipline would do it on POSIX.
   *
   * Windows has no VEOF: the console driver does not know the character, so
   * cmd.exe and PowerShell simply ignore it and the one keystroke that closes
   * a pane everywhere else does nothing. Typing `exit` works, which is the
   * tell -- the intent was never ambiguous, only unspoken. So slosh speaks it,
   * and that is exactly what it sends: the pane exits through the same door as
   * a typed `exit`, with the shell's own cleanup and exit status, and
   * `keep_dead` decides what is left behind. Closing the pane from outside
   * would skip all three.
   *
   * Off on POSIX, where the line discipline already does this properly and
   * an emulation could only get in the way -- ^D there is also EOF for `cat`
   * and every REPL, and none of that is ours to intercept. On for anyone who
   * wants the same key to mean the same thing on both.
   *
   * Narrow on purpose, because a wrong guess costs a pane. It fires only with
   * the shell itself in the foreground (no child process to be sending EOF
   * to), on the primary screen (in an editor or a pager ^D is half a page
   * down), and on an empty line -- where POSIX would send EOF rather than
   * discard what you typed. */
  bool ctrl_d_exits;

  /* Whether the program running in a pane may set that pane's shader chains
   * over OSC 5577. Off, because a program restyling the session it happens to
   * be running in is a hazard and not a feature (D13) -- and on for anyone
   * prototyping a chain, because the alternative is edit, save, look, and the
   * whole point of a colour pass is that you can see it.
   *
   * Scoped to the pane that asked: it cannot touch another pane, the chrome
   * outside itself, or anything the config said about everybody else. */
  bool in_band_shaders;

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

  /* Where projects live: `project_roots "~/dev" "~/work" depth=2`. A project is
   * a subdirectory of one of these with a `slosh.layout` in it -- or a
   * `.git`, which gets `project_layout` instead. Empty means the feature is
   * dormant and costs nothing.
   *
   * `depth` is levels below each root, and a directory that *is* a project is
   * never descended into, so 2 covers `~/dev/work/api` without walking any
   * checkout's insides. */
  char *project_roots[PROJECT_ROOTS_MAX];
  size_t nproject_roots;
  int project_depth;

  /* The layout a project with no file of its own opens as. Yours rather than a
   * guess about your stack: relative paths in it bind to whichever project is
   * being opened, so one file is the shape you start every project in. NULL is
   * one pane running your shell. */
  char *project_layout;

  /* Every file this config was built from, in the order they were read: the one
   * that was loaded and everything it included, whether or not each existed.
   * Kept because the watcher needs it — a theme you can include is a theme you
   * expect to reload when you save it — and recorded even for a file that is
   * not there, so a session started before you wrote one still notices it
   * appearing. */
  char *files[CONFIG_FILES_MAX];
  size_t nfiles;

  /* Everything this load had to complain about, in the order it found them,
   * each already carrying the file and line it happened at. A session shows the
   * first (there is one status line); `--check` shows them all, which is the
   * whole difference between a warning and a linter. */
  char msgs[CONFIG_MSGS_MAX][192];
  size_t nmsgs;

  /* Loader scratch: the file being read right now, so a complaint can say which
   * one it came from without every call site being handed the path. Meaningless
   * once loading has finished. */
  const char *loading;
} config_t;

/* Defaults, then <config dir>/config.kdl on top. Never fails: on a bad file
 * it keeps the defaults and writes the reason to `err`. */
void config_defaults(config_t *c);
bool config_load(config_t *c, const char *path, char *err, size_t errcap);
/* $SLOSH_CONFIG, else $XDG_CONFIG_HOME/slosh/config.kdl, else ~/.config/… */
const char *config_default_path(void);
/* Every complaint from the last load, oldest first: "config.kdl:12: ...".
 * Returns how many were written. */
size_t config_messages(const config_t *c, const char **out, size_t max);
void config_free(config_t *c);
/* The files this config was read from, oldest first: `files[0]` is the one that
 * was loaded, the rest are what it included. Returns how many were written. */
size_t config_files(const config_t *c, const char **out, size_t max);

/* Shader chains from text rather than from a file: one entry, several separated by
 * `;`, or a whole `shaders { ... }` block. Every shape a config file has, because
 * the point of using the config's syntax at a prompt is that the two directions
 * match -- what you prototype is what you paste, and what you paste is something
 * you can type back.
 *
 * Each pass goes where its own `where=` says; `default_chrome` is only what a pass
 * that keeps quiet means. Here because the parser belongs with the config: a chain
 * typed at a running pane and a chain in a config file have to mean the same
 * thing, or prototyping teaches you something that then does not work.
 *
 * Both arrays hold SHADE_MAX and `exprs` twice that; the compiled programs belong
 * to the caller, since a pane's chain outlives no load in particular. Returns how
 * many entries were understood, writing the first refusal to `err`. */
size_t config_parse_chain_doc(const char *text, color_t default_color,
                              bool default_chrome, shader_t *content,
                              size_t *ncontent, shader_t *chrome,
                              size_t *nchrome, expr_prog_t **exprs,
                              size_t *nexprs, char *err, size_t errcap);

/* The same, from a file: a `shaders { }` block as `contrib/chrome` and
 * `contrib/shaders` ship them, read with the parser that already knows this
 * format. Entries are routed to the two chains by their own `where=`, defaulting
 * to content as a config file does, so a preset lands where its author said.
 * Anything else at the top level is ignored -- hand it a whole config.kdl and
 * only the shaders apply. Both arrays hold SHADE_MAX; `exprs` holds twice that.
 * Returns how many entries were understood. */
size_t config_parse_chain_file(const char *path, color_t default_color,
                               shader_t *content, size_t *ncontent,
                               shader_t *chrome, size_t *nchrome,
                               expr_prog_t **exprs, size_t *nexprs, char *err,
                               size_t errcap);

/* Whether a name is one of the config's own settings. For the layout loader, which
 * shares this syntax and needs to tell a config apart from a session file -- one
 * list, so the two answers cannot disagree. */
bool config_is_setting(const char *name);

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
/* The name this action is written as in a config file. */
const char *config_action_name(action_t a);
/* The whole config as a file you could have written, with the values it
 * currently holds. Generated rather than kept on disk, because a checked-in
 * copy of the defaults is a second source of truth and drifts. Caller frees. */
char *config_render(const config_t *c);
/* The same, for a config nobody has edited: what a fresh install would do.
 * Caller frees. */
char *config_dump_defaults(void);
const char *config_action_group(action_t a);
/* "ctrl+a", "alt+left", "\\", "f" -> key + mods. False if unparseable. */
bool config_parse_chord(const char *text, int *out_key, uint16_t *out_mods);

#endif /* SLOSH_CONFIG_H */
