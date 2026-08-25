/* app.c internals, shared by the app_*.c compilation units and nobody
 * else: the tree, the tabs and the session struct itself, plus the
 * handful of helpers every unit leans on. Everything public is in
 * app.h; this header exists so one 7k-line file could become five. */
#ifndef SLOSH_APP_INTERNAL_H
#define SLOSH_APP_INTERNAL_H

#include "app.h"
#include "config.h"
#include "expr.h"
#include "shader.h"

/* A chain attached to one pane, with the programs its expressions compiled to
 * kept beside it. Together because they are one lifetime: freeing the chain
 * without the programs leaks, and freeing the programs without the chain leaves
 * shaders pointing at freed code. */
typedef struct {
  shader_t sh[SHADE_MAX];
  size_t n;
  expr_prog_t *exprs[SHADE_MAX];
  size_t nexprs;
} inband_chain_t;

struct node {
  enum { NODE_LEAF, NODE_SPLIT } kind;
  node_t *parent;
  rect_t rect; /* recomputed every layout pass; never trusted between them */

  /* Share of the parent split, in arbitrary units. Even splits are simply
   * equal weights, so resizing is not a special case of anything. */
  int weight;

  /* Put away: out of the layout, into a strip along the bottom, still running.
   * Intent, like the weight beside it and unlike the rect and the collapsed
   * flag below — layout_node must not clear this, or it would un-minimise
   * every pane on every frame. */
  bool minimized;

  /* Floating: out of the layout, drawn on top of the tiled panes and below
   * the modals. Same pattern as `minimized` — the leaf keeps its seat in the
   * tree, layout skips it, the siblings absorb its share, and un-floating
   * returns it home: same id, same place, nothing that referred to it has to
   * be told anything.
   *
   * `float_rect` is where it *wants* to be, in cells. Intent: what is drawn
   * each frame is this clamped to the tab's area, and the clamp never writes
   * back — shrink the terminal and the float is squeezed in, grow it again
   * and it is exactly where you put it. Zero-width means "never placed",
   * which the placement answers with a centred default.
   *
   * `raised` orders overlapping floats: highest paints last, so it is on
   * top. Stamped when a float is made and when a focused float is not the
   * top one — in layout(), once, rather than at each place focus can move. */
  bool floating;
  rect_t float_rect;
  uint32_t raised;

  /* leaf */
  pane_t *pane;
  uint32_t id;
  rect_t content; /* where the pane's cells go */

  /* Colour passes over this pane, applied in order: one chain for its contents
   * and one for its frame, because those are two rects and not one list with a
   * sort key. Sent in-band by the program inside the pane, which `shaders.md`
   * calls prototyping and D13 called a hazard -- both are true, so it takes
   * `in_band_shaders true` to be possible at all.
   *
   * Each chain owns the programs its expressions compiled to. The config owns
   * the ones a file produced and frees them on reload; these live exactly as
   * long as the chain that was sent, which is until the next one replaces it or
   * the pane goes away, and that is nobody else's schedule. */
  inband_chain_t content_chain, chrome_chain;
  char purpose[64];
  bool purpose_locked; /* declared by a layout: in-band cannot override */

  /* When this pane last became what it is, for `since` (D20). A *transition*
   * cannot be derived from the frame in front of you — the frame only says
   * what is true now — so this is remembered, and it is remembered in the one
   * place that decides the state, which is what keeps the timestamp and the
   * state it belongs to from disagreeing. `last_state` is the state it was in
   * when `state_since` was stamped; PSTATE_COUNT is "in none of them", which
   * is a state to have been in for a while like any other. */
  pane_state_t last_state;
  int64_t state_since;

  /* split */
  split_dir_t dir;
  node_t **kids;
  size_t nkids;

  /* recomputed every layout pass, like the rect: this subtree did not fit and
   * is drawn as a one-row header (D6). Never remembered between passes. */
  bool collapsed;
  bool hidden; /* a leaf inside a collapsed subtree: running, not drawn */
};

/* A tab is a layout tree and its focus. Panes in every tab keep running; only
 * the current tab is composed. */
/* Where a row boundary and a column boundary meet, and the boundaries that
 * meet there. Recomputed every frame with the layout it describes. */
typedef struct {
  rect_t r;
  uint32_t h_id[2]; /* row boundaries meeting here: these move vertically */
  size_t h_edge[2];
  size_t nh;
  uint32_t v_id[2]; /* column boundaries: these move horizontally */
  size_t v_edge[2];
  size_t nv;
} corner_t;

typedef struct {
  node_t *root;
  node_t *focus;
  /* Where the minimised panes are listed, or zero-sized when none are. Set by
   * the layout and consumed by the drawing, rather than each working the
   * geometry out for itself. */
  rect_t min_bar;
  /* The pane filling this tab on its own, or 0. Intent rather than derived
   * state, like focus: nothing about the tree says a pane is zoomed, you said
   * so. Kept per tab so zooming one does not disturb another. */
  uint32_t zoom;
  uint32_t id;
  char name[64];
  char purpose[64];
  bool purpose_locked;
} tab_t;

struct app {
  tab_t *tabs;
  size_t ntabs, tabcap, cur;
  uint32_t next_tab_id;
  uint32_t next_id;
  /* The last raise stamp handed out. A float whose `raised` equals this is
   * the top one, which is what lets the focus rule stamp without spinning
   * the counter every frame. */
  uint32_t raise_seq;
  uint16_t cols, rows;
  /* The client's cell size in pixels, as reported when it attached. Panes are
   * told so that a program can size an image, and so lib-vt can work out how
   * many cells an image covers when the program did not say. Defaulted rather
   * than zeroed: 0 means "no image can be placed", which is not a good thing
   * for a headless session or a terminal that will not say. */
  uint16_t cell_w, cell_h;
  bool prefix;
  bool quit;
  bool detach;
  /* One drag machine, two verbs: a title drags a pane onto another to swap
   * them, a gap between panes drags the boundary. Both are started by a press
   * on something the hit list says is draggable, so neither can disagree with
   * what is on screen. */
  struct {
    enum {
      DRAG_NONE,
      DRAG_TITLE,
      DRAG_EDGE,
      DRAG_SELECT,
      DRAG_BORDER,
      DRAG_TAB,
      /* The float verbs: the title row moves it, any other border cell
       * resizes from the edges under the grab. Their own kinds rather than
       * flags on DRAG_TITLE/DRAG_BORDER, because everything those two mean
       * on release — swap, split — is exactly what a float must not do. */
      DRAG_FLOAT_MOVE,
      DRAG_FLOAT_RESIZE,
    } kind;
    uint32_t src;    /* pane being dragged, or the split being resized */
    uint32_t target; /* pane under the pointer, for the drop highlight */
    /* ...or the tab under it, when a pane is dragged over the strip: the same
     * gesture with a different kind of destination. Never both -- the pointer is
     * over one thing, and two highlights would be two promises. */
    uint32_t tab_target;
    bool new_tab_target; /* the strip's `+`: a tab of its own */
    size_t edge;         /* which boundary of that split */
    uint16_t x, y;       /* where the pointer was at the last event */
    bool moved;          /* a press that never moves is a click */
    char side;           /* border press: 'l' 'r' 't' 'b' */
    /* The press landed on the rim rather than the handle. The rim's click
     * deliberately does nothing — only the handle splits — but a *drag* from
     * anywhere on a border moves the boundary, so the rim arms the same drag
     * and this is what keeps its click inert on release. */
    bool rim;
    /* Which of the target's drop zones the pointer is in, or 0 for the
     * centre. The centre is the swap; a side means "insert me beside the
     * target, on this side" — the drop grammar that turns dragging into
     * re-layout. */
    char drop_side;
    /* Which edges a float resize is holding (FEDGE_*): derived from where
     * the press landed on the frame, so a bottom corner is two edges and
     * follows the pointer on both axes. */
    uint8_t fedges;
    /* A corner drag carries the boundaries it is moving, not an index into
     * the corner list: that list is rebuilt from the layout every frame, and
     * the layout is the thing being changed. Halfway through a drag the
     * crossing you grabbed may be a different entry, or gone — an index would
     * quietly start moving somebody else's panes. */
    uint32_t c_h[2];
    size_t c_hedge[2];
    size_t c_nh;
    uint32_t c_v[2];
    size_t c_vedge[2];
    size_t c_nv;
  } drag;

  graphics_t *gfx; /* kitty images, and what the client has been told */

  /* Where the pointer is. Which border that *means* is derived during the
   * paint, from the rects painted in that same pass — remembering the answer
   * instead left the guide describing the layout from before a split, until
   * the mouse moved and corrected it. */
  uint16_t ptr_x, ptr_y;
  bool ptr_valid;
  int64_t ptr_still_since; /* when the pointer last stopped moving */

  /* Transient announcements: "copied 13 chars", a notification from a pane,
   * a config reload. They stack upward from the bottom right and expire on
   * their own, so nothing has to be dismissed. */
  struct {
    char text[128];
    int64_t until;
  } toasts[3];
  size_t ntoasts;

  /* What a selection put on the clipboard, kept for middle-click paste (the
   * X11 primary-selection habit) and handed to the client as OSC 52. */
  char *clipboard;
  char *clipboard_pending;

  /* The cheatsheet. An overlay like the finder, but a *modal*: it reads and
   * is dismissed, it never navigates, so anything you press puts it away. */
  bool help;

  /* The modal picker: a query, a filtered list, one selection. Two things
   * wear it -- the pane finder (tabs stop being navigation past about six)
   * and the command palette -- and they are one machine with a subject
   * rather than two that would drift apart, for the same reason the rename
   * editor is. Only one can be open, which is what makes sharing the query
   * and the selection honest rather than a saving. */
  enum { PICK_NONE = 0, PICK_FINDER, PICK_PALETTE, PICK_WORKSPACES } picker;
  /* What the project picker is listing: scanned once when it opens, because
   * draw_picker asks for its rows every frame and a readdir per repaint is a
   * filesystem walk at 120Hz. Once per opening is when the answer has to be
   * right, which is why nothing is kept for longer. */
  project_t *projects;
  size_t nprojects;
  char query[64];
  size_t sel;

  /* Renaming in place: the title cell becomes the editor, so the name is typed
   * where it will live rather than in a dialog somewhere else. A pane's title
   * and a tab's label are the same gesture on two different things, so this is
   * one editor with a subject rather than two machines that would drift. */
  enum { RENAME_NONE = 0, RENAME_PANE, RENAME_TAB, RENAME_PURPOSE } renaming;
  uint32_t rename_id;
  /* As wide as a pane title, so seeding the editor never truncates one (and so
   * never truncates one mid-UTF-8). */
  char rename_buf[256];
  /* The first click of a candidate double-click on a title. The *kind* is
   * remembered with it: pane ids and tab ids are separate sequences, so pane 2
   * and tab 2 both exist, and without this a click on one followed by a click
   * on the other reads as a double-click on neither. */
  int64_t name_click_ms;
  uint32_t name_click_id;

  /* The same idea for a pane's contents: a second click on the *same cell* of
   * the same pane selects the word there. Per cell as well as per pane, so
   * clicking two different words quickly selects the second one rather than
   * everything between them. */
  int64_t cell_click_ms;
  uint32_t cell_click_id;
  uint16_t cell_click_x, cell_click_y;
  int name_click_kind;
  /* Set while a layout is being built, by a `focus=true` pane, and consumed
   * by the tab that contains it: focus belongs to a tab, and the tab does not
   * exist yet when the pane declaring it is made. */
  node_t *restore_focus;
  /* Which tab a layout said was active, resolved after every tab exists --
   * the index is only meaningful once the array has stopped growing. */
  size_t restore_tab;

  /* Set for the duration of one app_apply_layout: every pane it builds starts
   * suspended whatever the file said. `open-workspace suspended:true` is the
   * "open ten projects, run zero processes" case, which is a different question
   * from the one a project's layout answered about which of *its* panes are
   * expensive -- and it has to be decided before anything spawns, because a pane
   * is created suspended or created running and there is no un-running one. */
  bool force_suspend;
  const char *const *argv;
  /* What this session is called. The server knows it because it opened the
   * socket under that name; the app only knows it because it is worth saying
   * out loud when several are running. */
  char session[64];
  /* The last layout pass made this tab a list rather than a layout (D6). Same
   * rule as `animating`: derived every pass, never remembered across one. */
  bool flattened;
  /* the screen we last composed into: its hit list is what a click resolves
   * against, so routing can never consult geometry the user never saw */
  corner_t corners[16];
  size_t ncorners;
  const screen_t *painted;
  /* Something on the frame we last composed is animated: a shader whose amount
   * reads the clock ran over it. Derived every frame from the chains actually
   * applied, like the rect and the collapsed flag and for the same reason —
   * remembering "this session animates" would keep a clock running for a pulse
   * that was hung off a state no pane is in any more. */
  bool animating;

  /* The logo splash: shown until this instant, 0 for not at all. Set when a
   * client attaches (app_splash), ended early by any key or click. The
   * timestamp doubles as the seed that picks the colour effect and the
   * particle motion this one wears, unless splash_fx / splash_motion (>= 0,
   * from the control API) already chose. */
  int64_t splash_until;
  int splash_fx;
  int splash_motion;
};

/* One config per process, owned by app.c (ensure_config/app_reload_config
 * write it); everything else reads it. The macros are the palette spelled
 * short, because chrome-drawing code says these names hundreds of times. */
extern config_t CFG;

#define WEIGHT_UNIT 1000
#define WEIGHT_MIN 150  /* a pane can be squeezed, not squeezed out */
#define WEIGHT_STEP 120 /* one keyboard nudge */

/* Which edges of a float a resize is moving. */
#define FEDGE_L 1u
#define FEDGE_R 2u
#define FEDGE_T 4u
#define FEDGE_B 8u

#define MIN_PANE_COLS (CFG.min_pane_cols)
#define MIN_PANE_ROWS (CFG.min_pane_rows)
#define FRAME_FOCUS (CFG.frame_focus)
#define FRAME_IDLE (CFG.frame_idle)
#define TITLE_FOCUS (CFG.title_focus)
#define BTN_FG (CFG.button_fg)
#define BTN_BG (CFG.button_bg)
#define BTN_BG_IDLE (CFG.button_bg_idle)
#define TITLE_IDLE (CFG.title_idle)
#define GUIDE (CFG.guide)
#define RESIZE_C (CFG.resize)
#define DROP_C (CFG.drop_target)
#define SCROLL_FG (CFG.scroll_fg)
#define SCROLL_BG (CFG.scroll_bg)
#define HEADER (CFG.header)
#define HEADER_HOVER (CFG.header_hover)
#define HEADER_HOVER_TITLE (CFG.header_hover_title)
#define TAB_ACTIVE_FG (CFG.tab_active_fg)
#define TAB_ACTIVE_BG (CFG.tab_active_bg)
#define TAB_ACTIVE_HOVER_FG (CFG.tab_active_hover_fg)
#define TAB_IDLE (CFG.tab_idle)
#define TAB_HOVER (CFG.tab_hover)
#define PREFIX_FG (CFG.prefix_fg)
#define PREFIX_BG (CFG.prefix_bg)
#define TAB_COUNT (CFG.tab_count)
#define STATUS_C (CFG.status)
#define STATUS_STATE (CFG.status_state)
#define FINDER_FG (CFG.finder_fg)
#define FINDER_BG (CFG.finder_bg)
#define FINDER_SEL_FG (CFG.finder_sel_fg)
#define FINDER_SEL_BG (CFG.finder_sel_bg)
#define TOAST_FG (CFG.toast_fg)
#define TOAST_BG (CFG.toast_bg)
#define RENAME_FG (CFG.rename_fg)
#define RENAME_BG (CFG.rename_bg)
#define BELL_C (CFG.bell)
#define PANE_BTN (CFG.pane_button)
#define PANE_BTN_HOVER (CFG.pane_button_hover)
#define MINBAR (CFG.minbar)
#define MINBAR_HOVER (CFG.minbar_hover)
#define HINT_C (CFG.hint)
#define DEAD_C (CFG.dead)
#define MODAL_FG (CFG.modal_fg)
#define MODAL_BG (CFG.modal_bg)
#define MODAL_BORDER (CFG.modal_border)
#define MODAL_TITLE (CFG.modal_title)
#define MODAL_BTN (CFG.modal_button)
#define MODAL_BTN_HOVER (CFG.modal_button_hover)

static const color_t NO_COLOR = {0};

/* The current tab: small enough to live here, called from everywhere. */
static inline tab_t *cur(app_t *a) { return &a->tabs[a->cur]; }

typedef void (*leaf_fn)(node_t *, void *);

/* Formerly static in app.c; shared across the app_*.c units since the
 * split. Grouped by the unit that defines them. */

/* src/app.c */
const char *const *default_argv(app_t *a);
uint16_t cells(const char *str);
void exit_words(const pane_t *p, char *out, size_t cap);
node_t *leaf_new(app_t *a);
node_t *leaf_new_ex(app_t *a, const char *const argv[], const char *cwd,
                    bool suspended, const char *label);
void node_free(node_t *n);
int64_t now_ms_(void);
bool ptr_on(const app_t *a, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
bool run_action(app_t *a, action_t act);
void sanitise_purpose(const char *in, char *out, size_t cap);
tab_t *tab_add(app_t *a, const char *name);
size_t tab_of(app_t *a, node_t *n);
void walk_all(app_t *a, leaf_fn fn, void *ud);

/* src/app_layout.c */
uint16_t eff_gap(split_dir_t dir);
rect_t app_tab_area(app_t *a);
void close_leaf(app_t *a, node_t *leaf);
node_t *first_leaf_of(node_t *n);
void focus_dir(app_t *a, int dx, int dy);
void layout(app_t *a);
node_t *pane_by_id(app_t *a, uint32_t id);
split_dir_t side_dir(char side);
bool split_fits(node_t *leaf, split_dir_t dir);
void split_focus(app_t *a, split_dir_t dir);
void split_focus_auto(app_t *a);
void split_focus_ui(app_t *a, split_dir_t dir);
void transfer_weight(node_t *from, node_t *to, int amount);
void float_move(app_t *a, node_t *n, int dx, int dy);
void float_resize(app_t *a, node_t *n, unsigned edges, int dx, int dy);
bool focus_float_move(app_t *a, int dx, int dy);
bool focus_float_grow(app_t *a, int sign);
uint8_t float_edges_at(const node_t *n, uint16_t mx, uint16_t my);

/* src/app_ui.c */
void picker_accept(app_t *a, const char *action);
void purpose_begin(app_t *a, uint32_t id);
void rename_begin(app_t *a, uint32_t id);
void rename_tab_begin(app_t *a, uint32_t id);

void draw_splash(app_t *a, screen_t *s);

/* round two */
void drop_pane_on_strip(app_t *a);
void focus_next(app_t *a);
const char *live_cwd(const pane_t *p, char *buf, size_t cap);
void move_tab(app_t *a, size_t from, size_t to);
bool picker_key(app_t *a, const input_event_t *ev);
bool push_pane_a_tab(app_t *a, bool forward);
void rename_end(app_t *a, bool keep);
bool rename_key(app_t *a, const input_event_t *ev);
void resize_focus(app_t *a, int dx, int dy);
void rotate_layout_ui(app_t *a);
void split_node(app_t *a, node_t *leaf, split_dir_t dir, bool before);
tab_t *tab_by_id(app_t *a, uint32_t id);
size_t tab_index(app_t *a, uint32_t id);

bool swap_panes(app_t *a, uint32_t id_a, uint32_t id_b);
bool insert_pane_beside(app_t *a, uint32_t src_id, uint32_t dst_id, char side);
struct byid {
  uint32_t id;
  node_t *found;
};

/* round three */
void chain_clear(inband_chain_t *c);
void walk(node_t *n, leaf_fn fn, void *ud);
void byid_cb(node_t *n, void *ud);
void find_corners(app_t *a);
void ensure_config(void);
void draw_toasts(app_t *a, screen_t *s);
void draw_tab_strip(app_t *a, screen_t *s);
void draw_status_line(app_t *a, screen_t *s);
void draw_node(app_t *a, screen_t *s, node_t *n);
void draw_min_bar(app_t *a, screen_t *s);
void draw_corners(app_t *a, screen_t *s);
void draw_floats(app_t *a, screen_t *s);
void draw_compact_lines(app_t *a, screen_t *s);
void draw_resize_hints(app_t *a, screen_t *s);

size_t count_leaves(node_t *n);
size_t collect_minimized(node_t *n, node_t **out, size_t cap, size_t k);
size_t collect_floating(node_t *n, node_t **out, size_t cap, size_t k);
size_t collect_leaves(node_t *n, node_t **out, size_t cap, size_t k);

#endif /* SLOSH_APP_INTERNAL_H */
