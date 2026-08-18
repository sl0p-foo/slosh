/* The layout tree, focus, chrome, and what keys do. See app.h. */
#define _GNU_SOURCE
#include "app.h"

#include <ghostty/vt.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expr.h"
#include "json.h"
#include "version.h"
#include "graphics.h"
#include "kdl.h"

/* ---- config ------------------------------------------------------------- */

#include "config.h"

/* One config per process. The server owns the session, and a session has one
 * look; `reload` re-reads it in place, which is why nothing caches a colour. */
static config_t CFG;
static bool CFG_LOADED = false;
/* What the config in force complained about while loading, or "". A complaint
 * is not a failure: an include that is not there, a shader nobody has heard of,
 * a binding that does not parse — the rest of the file applied and the session
 * is running (D9). But dropping it on the floor is how a mistyped theme name
 * turns into ten minutes of wondering, so it is kept for whoever can say it out
 * loud. The front end toasts it; the log gets it either way. */
static char CFG_COMPLAINT[256];

static void ensure_config(void) {
  if (CFG_LOADED) return;
  config_defaults(&CFG);
  char err[256] = {0};
  const char *path = config_default_path();
  if (!config_load(&CFG, path, err, sizeof err)) {
    /* A missing file is the normal case; a broken one is worth a line in the
     * log, and in both the compiled-in defaults stand (fail open). */
    if (access(path, R_OK) == 0) {
      fprintf(stderr, "sl0ppty: %s: %s\n", path, err[0] ? err : "parse error");
      snprintf(CFG_COMPLAINT, sizeof CFG_COMPLAINT, "%s",
               err[0] ? err : "config parse error");
    }
  } else if (err[0]) {
    fprintf(stderr, "sl0ppty: %s: %s\n", path, err);
    snprintf(CFG_COMPLAINT, sizeof CFG_COMPLAINT, "%s", err);
  }
  CFG_LOADED = true;
}

const char *app_config_complaint(void) {
  ensure_config();
  return CFG_COMPLAINT;
}

bool app_reload_config(char *err, size_t errcap) {
  config_t fresh;
  config_defaults(&fresh);
  bool ok = config_load(&fresh, config_default_path(), err, errcap);
  if (!ok) {
    config_free(&fresh);
    return false; /* keep what works */
  }
  config_free(&CFG);
  CFG = fresh;
  CFG_LOADED = true;
  /* Whatever the new file had to say about itself, including nothing. */
  snprintf(CFG_COMPLAINT, sizeof CFG_COMPLAINT, "%s", err && err[0] ? err : "");
  return true;
}

/* Which files the config in force was read from. The server watches every one
 * of them, so a theme that can be included is a theme that reloads when you
 * save it — asked of the app rather than read from a config the front ends do
 * not own. */
size_t app_config_files(const char **out, size_t max) {
  ensure_config();
  return config_files(&CFG, out, max);
}

#define WEIGHT_UNIT 1000
#define WEIGHT_MIN 150  /* a pane can be squeezed, not squeezed out */
#define WEIGHT_STEP 120 /* one keyboard nudge */

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

/* ---- tree --------------------------------------------------------------- */

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
};

static tab_t *cur(app_t *a) { return &a->tabs[a->cur]; }

/* Is the pointer sitting on this rect right now?
 *
 * Always asked with the rect that is about to be registered as the hit, so a
 * thing that lights up and the thing that would be clicked cannot drift apart.
 * A pointer already carrying something is busy and lights nothing. */
/* Display columns of a chrome string, from the terminal's own width table.
 *
 * This counted codepoints until a two-column bell mark shifted a pane's title
 * row: booking one column for a glyph drawn in two moves everything after it,
 * and every length in this file -- title budgets, button positions, the hit
 * rects that must agree with them -- is computed from here. */
static uint16_t cells(const char *str) { return screen_cells(str); }

static bool ptr_on(const app_t *a, uint16_t x, uint16_t y, uint16_t w,
                   uint16_t h) {
  return a->ptr_valid && a->drag.kind == DRAG_NONE && a->ptr_x >= x &&
         a->ptr_x < x + w && a->ptr_y >= y && a->ptr_y < y + h;
}

/* "exited", "exited: status 3", "exited: signal 9" — the same words wherever
 * the fact is reported, so the status line, the pane's own frame and the line
 * left in its backlog cannot describe one death three ways. */
static void exit_words(const pane_t *p, char *out, size_t cap) {
  int code = 0;
  bool sig = false;
  if (!pane_exit(p, &code, &sig) || (!sig && code == 0))
    snprintf(out, cap, "exited");
  else
    snprintf(out, cap, "exited: %s %d", sig ? "signal" : "status", code);
}

typedef void (*leaf_fn_fwd)(node_t *, void *);
static void walk_all(app_t *a, leaf_fn_fwd fn, void *ud);

struct bypane {
  const pane_t *pane;
  node_t *found;
};
static void bypane_cb(node_t *n, void *ud) {
  struct bypane *b = ud;
  if (n->pane == b->pane) b->found = n;
}

/* OSC 5577 verbs pane.c does not own. `purpose` arrives from a pane's own
 * output, so it is in-band by definition and can never be declared: a pane
 * that prints one is asking, not telling (D8). */
/* `shader;<where>;<chain>` from the program in the pane: `where` is `content` or
 * `chrome`, and the chain is the rest of the payload *verbatim*, `;` and all,
 * because a chain is several entries separated by `;` and escaping the one
 * character the syntax uses would make a pasteable line unpasteable.
 *
 * Answered either way, on the pane's own stdin: `shader;ok` or
 * `shader;error;<why>`. A prototyping loop that gets no answer cannot tell "you
 * typed it wrong" from "this build does not have it", and a program asking for
 * something the session refuses deserves to hear so rather than watch nothing
 * happen. */
static void on_pane_shader(app_t *a, pane_t *p, const char *payload) {
  const char *semi = strchr(payload, ';');
  size_t wlen = semi ? (size_t)(semi - payload) : strlen(payload);
  bool chrome = wlen == 6 && memcmp(payload, "chrome", 6) == 0;
  bool content = wlen == 7 && memcmp(payload, "content", 7) == 0;

  char reply[256];
  struct bypane b = {p, NULL};
  walk_all(a, bypane_cb, &b);
  if (!b.found) return;

  /* Naming no rect means all of them: `shader;` puts the pane back in one
   * exchange. The degenerate case of "which rect" rather than a verb of its own,
   * because `shader;chrome;` and `shader;content;` already mean "this rect, this
   * chain" and "" is the honest name for both of them. */
  if (!wlen) {
    app_clear_pane_shaders(a, b.found->id);
    snprintf(reply, sizeof reply, "\033]5577;1;shader-reply;ok\033\\");
    pane_write(p, reply, strlen(reply));
    return;
  }
  if (!chrome && !content) {
    snprintf(
        reply, sizeof reply,
        "\033]5577;1;shader-reply;error;where must be content or chrome\033\\");
    pane_write(p, reply, strlen(reply));
    return;
  }

  char err[192] = {0};
  const char *text = semi ? semi + 1 : "";
  size_t nchrome = 0, ncontent = 0;
  bool ok = app_set_pane_shaders(a, b.found->id, chrome, text, &nchrome,
                                 &ncontent, err, sizeof err);
  if (ok)
    snprintf(reply, sizeof reply,
             "\033]5577;1;shader-reply;ok;%zu chrome, %zu content\033\\",
             nchrome, ncontent);
  else
    snprintf(reply, sizeof reply, "\033]5577;1;shader-reply;error;%s\033\\",
             err[0] ? err : "refused");
  pane_write(p, reply, strlen(reply));
}

/* `shader-load;<path>`: the file's `shaders { }` block, on this pane. Answered with
 * how much of it ran, because "ok" alone cannot tell a preset that filled both
 * chains from a file whose entries were all dropped for one bad word. */
static void on_pane_shader_load(app_t *a, pane_t *p, const char *payload) {
  struct bypane b = {p, NULL};
  walk_all(a, bypane_cb, &b);
  if (!b.found) return;

  char err[192] = {0}, reply[320];
  size_t nchrome = 0, ncontent = 0;
  bool ok = app_load_pane_shaders(a, b.found->id, payload, &nchrome, &ncontent,
                                  err, sizeof err);
  if (ok)
    snprintf(reply, sizeof reply,
             "\033]5577;1;shader-reply;ok;%zu chrome, %zu content\033\\",
             nchrome, ncontent);
  else
    snprintf(reply, sizeof reply, "\033]5577;1;shader-reply;error;%s\033\\",
             err[0] ? err : "refused");
  pane_write(p, reply, strlen(reply));
}

static void on_pane_osc(pane_t *p, const char *verb, const char *payload,
                        void *ud) {
  app_t *a = ud;
  if (strcmp(verb, "shader") == 0) {
    on_pane_shader(a, p, payload);
    return;
  }
  if (strcmp(verb, "shader-load") == 0) {
    on_pane_shader_load(a, p, payload);
    return;
  }
  if (strcmp(verb, "purpose") != 0) return;
  struct bypane b = {p, NULL};
  walk_all(a, bypane_cb, &b);
  if (b.found) app_set_pane_purpose(a, b.found->id, payload, false);
}

static int64_t now_ms_(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void app_toast(app_t *a, const char *text) {
  if (!text || !*text) return;
  size_t max = sizeof a->toasts / sizeof *a->toasts;
  if (a->ntoasts == max) { /* drop the oldest */
    memmove(&a->toasts[0], &a->toasts[1], (max - 1) * sizeof a->toasts[0]);
    a->ntoasts--;
  }
  snprintf(a->toasts[a->ntoasts].text, sizeof a->toasts[0].text, "%s", text);
  a->toasts[a->ntoasts].until = now_ms_() + CFG.toast_ms;
  a->ntoasts++;
}

static void toasts_expire(app_t *a) {
  int64_t now = now_ms_();
  size_t keep = 0;
  for (size_t i = 0; i < a->ntoasts; i++)
    if (a->toasts[i].until > now) a->toasts[keep++] = a->toasts[i];
  a->ntoasts = keep;
}

/* When something needs repainting on its own: a toast expiring, a hover guide
 * arming, or an animated shader — the three things on screen that change
 * without an event behind them. Without the second one the guide would appear
 * on the next event rather than when the dwell is up, which for a resting
 * pointer means "never"; without the third a pulse would only advance when
 * something else happened to cause a frame, which for an idle pane means a
 * border frozen mid-pulse. */
int app_next_deadline_ms(app_t *a) {
  int64_t soonest = -1;
  for (size_t i = 0; i < a->ntoasts; i++)
    if (soonest < 0 || a->toasts[i].until < soonest)
      soonest = a->toasts[i].until;

  if (a->ptr_valid && a->painted) {
    int64_t due = a->ptr_still_since + CFG.hover_delay_ms;
    if (due > now_ms_()) {
      /* Everything that arms on dwell has to be listed here, or it would only
       * appear when some unrelated event happened to repaint the frame. */
      const char *action = hit_test(&a->painted->hits, a->ptr_x, a->ptr_y);
      bool on_border = action && (strncmp(action, "border:", 7) == 0 ||
                                  strncmp(action, "brim:", 5) == 0 ||
                                  strncmp(action, "title:", 6) == 0 ||
                                  strncmp(action, "edge:", 5) == 0 ||
                                  strncmp(action, "corner:", 7) == 0);
      if (on_border && (soonest < 0 || due < soonest)) soonest = due;
    }
  }

  /* An animated shader ran over the last frame, so the next thing that changes
   * is the clock. Bounded by a config knob rather than painting flat out: this
   * is a terminal, and 20fps of one pane's border is not worth a core. The
   * flag comes from the frame we composed, so the cost lands only while
   * something animated is actually on screen. */
  if (a->animating && CFG.anim_ms) {
    int64_t due = now_ms_() + CFG.anim_ms;
    if (soonest < 0 || due < soonest) soonest = due;
  }

  if (soonest < 0) return -1;
  int64_t in = soonest - now_ms_();
  return in <= 0 ? 0 : (int)in;
}

size_t app_toast_count(app_t *a) {
  toasts_expire(a);
  return a->ntoasts;
}

static size_t count_leaves(node_t *n); /* all defined with the layout */
static size_t collect_minimized(node_t *n, node_t **out, size_t cap, size_t k);
static size_t collect_leaves(node_t *n, node_t **out, size_t cap, size_t k);

/* What the thing under the pointer would do, in a word.
 *
 * Read off the hit list rather than tracked, so anything that registers a hit
 * gets a hint by being listed here and nothing has to remember to raise one.
 * The status line is drawn after everything else has registered, so the list
 * is complete by the time this is asked. */
static const char *hint_for(app_t *a, const char *action) {
  if (!action) return NULL;
  uint32_t id = 0;
  const char *colon = strchr(action, ':');
  if (colon) id = (uint32_t)strtoul(colon + 1, NULL, 10);

  if (strncmp(action, "minimize:", 9) == 0) return "minimise";
  if (strncmp(action, "zoom:", 5) == 0)
    return app_pane_zoomed(a, id) ? "back to the layout" : "fill the tab";
  if (strncmp(action, "close:", 6) == 0) return "close this pane";
  if (strncmp(action, "rerun:", 6) == 0) return "run the command again";
  if (strncmp(action, "scrollbottom:", 13) == 0) return "back to the bottom";
  if (strncmp(action, "panetitle:", 10) == 0)
    return "double-click to rename \u00b7 drag to move";
  if (strncmp(action, "title:", 6) == 0)
    /* The handle says both things it can do; the rest of the row only drags. */
    return strchr(action + 6, ':') ? "drag to move \u00b7 click to split up"
                                   : "drag to move";
  /* The rim gets no caption on purpose: it is the part of the edge that does
   * nothing, and the guide it arms is already pointing at the part that does. */
  if (strncmp(action, "brim:", 5) == 0) return NULL;
  if (strncmp(action, "border:", 7) == 0) {
    const char *side = strrchr(action, ':');
    switch (side && side[1] ? side[1] : 0) {
    case 'l': return "click to split left";
    case 'r': return "click to split right";
    case 't': return "click to split up";
    default: return "click to split down";
    }
  }
  if (strncmp(action, "edge:", 5) == 0) return "drag to resize";
  if (strncmp(action, "corner:", 7) == 0) return "drag to resize both ways";
  if (strncmp(action, "focus:", 6) == 0) return "open this pane";
  if (strncmp(action, "find:", 5) == 0) return "go to this pane";
  if (strncmp(action, "open:", 5) == 0) return "go to this project";
  /* Not "click to switch": that is the one of the three nobody needs telling.
   * Same rule as a pane's title, which advertises the rename and the drag. */
  if (strncmp(action, "tab:", 4) == 0)
    return "double-click to rename \u00b7 drag to reorder";
  if (strcmp(action, "newtab") == 0) return "new tab";
  return NULL; /* a pane's own content, and anything not worth a word */
}

/* The line along the bottom: what you are looking at, rather than what you
 * could switch to. The strip along the top already answers "which tab" and
 * "how many panes"; repeating that here would spend a row saying it twice.
 *
 * So this one is about the focused pane: which session it lives in, which tab,
 * what it calls itself, and whether it is in a state worth knowing about —
 * scrolled back, on an alternate screen, or not started yet. Those last three
 * are the ones you can otherwise only discover by being surprised. */
/* The minimised panes, listed along one row. Shaped like the tab strip because
 * it is the same kind of thing: a row of names you click to go somewhere. */
static void draw_min_bar(app_t *a, screen_t *s) {
  if (!a->ntabs) return;
  rect_t bar = cur(a)->min_bar;
  if (!bar.w || !bar.h) return;

  node_t *mins[64];
  size_t nmin = collect_minimized(cur(a)->root, mins, 64, 0);
  if (!nmin) return;

  uint16_t x = bar.x;
  uint16_t right = (uint16_t)(bar.x + bar.w);

  /* A legend, so a row of bare names is not a mystery. */
  x = (uint16_t)(x +
                 screen_text(s, x, bar.y, CFG.min_mark, MINBAR, NO_COLOR, 0));
  x = (uint16_t)(x + 1);

  for (size_t i = 0; i < nmin && x < right; i++) {
    const char *nm = pane_title(mins[i]->pane);
    /* A pane you cannot see is exactly the one a bell is for, so the mark
     * comes along and keeps its own colour rather than the row's. */
    bool rang = CFG.bell_indicator && pane_bell(mins[i]->pane);

    char label[64];
    snprintf(label, sizeof label, " %s", nm && *nm ? nm : "pane");
    uint16_t lw = cells(label);
    uint16_t bw = rang ? (uint16_t)(1 + cells(CFG.bell_mark)) : 0;
    uint16_t total = (uint16_t)(lw + bw + 1); /* and the gap to the next one */
    if ((uint16_t)(x + total) > right) break;

    screen_text(s, x, bar.y, label, MINBAR, NO_COLOR, 0);
    /* Lit as one target, since the whole entry is one target. */
    if (ptr_on(a, x, bar.y, total, 1))
      screen_text(s, x, bar.y, label, MINBAR_HOVER, NO_COLOR, ATTR_BOLD);
    if (rang) {
      char mark[24];
      snprintf(mark, sizeof mark, " %s", CFG.bell_mark);
      screen_text(s, (uint16_t)(x + lw), bar.y, mark, BELL_C, NO_COLOR,
                  ATTR_BOLD);
    }
    char action[48];
    snprintf(action, sizeof action, "focus:%u", mins[i]->id);
    hit_add(&s->hits, x, bar.y, total, 1, action);
    x = (uint16_t)(x + total);
  }
}

static void draw_status_line(app_t *a, screen_t *s) {
  if (!CFG.status_line || s->rows < 3) return;
  uint16_t y = (uint16_t)(s->rows - CFG.gap - 1);
  uint16_t x = CFG.status_pad;
  uint16_t right =
      (uint16_t)(s->cols > CFG.status_pad ? s->cols - CFG.status_pad : 0);
  if (right <= x) return;

  node_t *f = a->ntabs ? cur(a)->focus : NULL;
  /* The leftmost column the right-hand block ends up occupying, so the middle
   * can tell whether it would run into it. */
  uint16_t right_used = right;

  /* Right side first, so a long name can never push the state off the end —
   * the same budgeting rule the tab strip and the pane frame both use.
   *
   * The count goes hard against the edge with the state inboard of it, which
   * is how the strip above orders its own pair: the number is always there,
   * the mode only sometimes, and a thing that comes and goes should not move
   * a thing that does not. This count is *this tab's*; the strip above counts
   * the whole session, which is why they can disagree. */
  if (a->ntabs && cur(a)->root) {
    /* Which of this tab's panes, out of how many — the count alone answers
     * "how big is this tab" and leaves "where am I in it" to be worked out by
     * counting frames, which is the question you actually have when a tab has
     * collapsed into a list and only one of them is open. */
    node_t *leaves[64];
    size_t nl = collect_leaves(cur(a)->root, leaves, 64, 0);
    size_t idx = 0;
    for (size_t i = 0; i < nl; i++)
      if (leaves[i] == f) idx = i + 1;

    char cnt[40];
    if (idx)
      snprintf(cnt, sizeof cnt, "pane %zu/%zu", idx, nl);
    else /* nothing focused: still say how many there are */
      snprintf(cnt, sizeof cnt, "%zu pane%s", nl, nl == 1 ? "" : "s");
    uint16_t cw = (uint16_t)strlen(cnt);
    if (right > x + cw + 2) {
      screen_text(s, (uint16_t)(right - cw), y, cnt, STATUS_C, NO_COLOR, 0);
      right_used = (uint16_t)(right - cw);
      right = (uint16_t)(right - cw - 2);
    }
  }

  char ind[64] = {0};
  if (f) {
    /* Ordered by what you would rather be told. A pane whose program is gone
     * is over, and that outranks every arrangement it happens to be in. */
    if (pane_suspended(f->pane)) {
      snprintf(ind, sizeof ind, "not started");
    } else if (!pane_alive(f->pane)) {
      exit_words(f->pane, ind, sizeof ind);
    } else if (app_pane_zoomed(a, f->id)) {
      snprintf(ind, sizeof ind, "zoomed");
    } else if (pane_scrolled(f->pane)) {
      uint32_t above = 0, total = 0;
      pane_scroll_pos(f->pane, &above, &total);
      snprintf(ind, sizeof ind, "\u25b2 %u scrolled",
               total > above ? total - above : 0);
    } else if (pane_alt_screen(f->pane)) {
      snprintf(ind, sizeof ind, "alt screen");
    }
  }
  if (ind[0]) {
    /* Columns, not bytes: the only non-ASCII here is the arrow, three bytes
     * wide and one column, and strlen would hold the whole indicator two
     * columns further from the edge than it needed to be. */
    uint16_t iw = cells(ind);
    if (right > x + iw + 2) {
      screen_text(s, (uint16_t)(right - iw), y, ind, STATUS_STATE, NO_COLOR,
                  ATTR_BOLD);
      right_used = (uint16_t)(right - iw);
      right = (uint16_t)(right - iw - 2);
    }
  }

  char line[256];
  size_t n = 0;
  if (a->session[0])
    n += (size_t)snprintf(line + n, sizeof line - n, "%s", a->session);
  if (a->ntabs) {
    const tab_t *t = &a->tabs[a->cur];
    const char *tn = t->name[0] ? t->name : (t->purpose[0] ? t->purpose : NULL);
    if (tn)
      n += (size_t)snprintf(line + n, sizeof line - n, "%s%s",
                            n ? " \u00b7 " : "", tn);
  }
  if (f) {
    const char *pt = pane_title(f->pane);
    const char *pp = f->purpose[0] ? f->purpose : NULL;
    if (pp)
      n += (size_t)snprintf(line + n, sizeof line - n, "%s%s",
                            n ? " \u00b7 " : "", pp);
    else if (pt && *pt)
      n += (size_t)snprintf(line + n, sizeof line - n, "%s%s",
                            n ? " \u00b7 " : "", pt);
  }
  if (n > (size_t)(right - x)) line[right - x] = 0;
  if (n) screen_text(s, x, y, line, STATUS_C, NO_COLOR, 0);

  /* What goes between the two ends: the hint for whatever the pointer is on,
   * and when there is none, which sl0ppty this is. The hint wins because it is
   * about right now and the banner is about always; the banner takes the slot
   * back the moment the pointer moves off, which is most of the time.
   *
   * Centred on the row, always, and not in whatever space the two ends have
   * left over. Centring it in the gap meant it moved whenever a pane title or
   * a state indicator changed length, so the one thing on the line you might
   * want to glance at was never twice in the same place. A fixed position you
   * can find without looking beats a tidy one you cannot.
   *
   * And when the row is too full for it there, it is not drawn. Not squeezed,
   * not slid along, not written over the ends: those say what session and pane
   * you are in, which is worth more than either a hint you can get again by
   * hovering or a version you can get again by looking a moment later. The
   * middle is the part that can afford to disappear. */
  const char *middle = NULL;
  color_t middle_fg = HINT_C;
  if (CFG.hints && a->painted && a->ptr_valid)
    middle = hint_for(a, hit_test(&s->hits, a->ptr_x, a->ptr_y));
  if (!middle && CFG.version_banner) {
    /* Quieter than a hint: this is ambient, and a hint is an answer to
     * something you are doing. */
    middle = "sl0ppty " SL0PPTY_VERSION;
    middle_fg = STATUS_C;
  }
  if (!middle) return;

  uint16_t hw = cells(middle);
  int from = ((int)s->cols - (int)hw) / 2;
  uint16_t left_end = (uint16_t)(x + cells(line));
  /* One blank column of clearance on each side, so "does not overlap" also
   * means "does not look like it does". */
  if (from < (int)left_end + 1) return;
  if ((uint16_t)(from + hw) + 1 > right_used) return;
  screen_text(s, (uint16_t)from, y, middle, middle_fg, NO_COLOR, 0);
}

static void draw_toasts(app_t *a, screen_t *s) {
  toasts_expire(a);
  if (!a->ntoasts) return;
  uint16_t bottom = (uint16_t)(s->rows > 1 ? s->rows - 1 : 0);
  for (size_t i = 0; i < a->ntoasts; i++) {
    const char *text = a->toasts[a->ntoasts - 1 - i].text;
    char line[160];
    snprintf(line, sizeof line, " %s ", text);
    uint16_t w = (uint16_t)strlen(line);
    if (w >= s->cols) w = (uint16_t)(s->cols - 1);
    uint16_t y = (uint16_t)(bottom - i);
    if (y >= s->rows) break;
    uint16_t x = (uint16_t)(s->cols - w - CFG.gap * CFG.gap_aspect);
    char clipped[160];
    snprintf(clipped, sizeof clipped, "%.*s", (int)w, line);
    screen_text(s, x, y, clipped, TOAST_FG, TOAST_BG, ATTR_BOLD);
  }
}

static void set_clipboard(app_t *a, char *text) {
  if (!text) return;
  if (!*text) {
    free(text);
    return;
  }
  free(a->clipboard);
  a->clipboard = text;
  {
    size_t n = strlen(text);
    char msg[64];
    snprintf(msg, sizeof msg, "copied %zu char%s", n, n == 1 ? "" : "s");
    app_toast(a, msg);
  }
  free(a->clipboard_pending);
  a->clipboard_pending = strdup(text); /* the client still has to be told */
}

static void on_pane_clipboard(pane_t *p, char *text, void *ud) {
  set_clipboard((app_t *)ud, text);
}

static void on_pane_notify(pane_t *p, const char *title, const char *body,
                           void *ud) {
  char msg[128];
  if (title && *title && body && *body)
    snprintf(msg, sizeof msg, "%s: %s", title, body);
  else
    snprintf(msg, sizeof msg, "%s",
             (title && *title) ? title
             : body            ? body
                               : "");
  app_toast((app_t *)ud, msg);
}

char *app_take_clipboard(app_t *a) {
  char *out = a->clipboard_pending;
  a->clipboard_pending = NULL;
  return out;
}

const char *app_clipboard(const app_t *a) { return a->clipboard; }

static node_t *leaf_new_ex(app_t *a, const char *const argv[], const char *cwd,
                           bool suspended, const char *label) {
  pane_t *p = pane_new_ex(argv, 1, 1, cwd, suspended, label);
  if (!p) return NULL;
  node_t *n = calloc(1, sizeof *n);
  n->kind = NODE_LEAF;
  n->weight = WEIGHT_UNIT;
  n->pane = p;
  n->id = ++a->next_id;
  pane_set_osc_handler(p, on_pane_osc, a);
  pane_set_clipboard_handler(p, on_pane_clipboard, a);
  pane_set_notify_handler(p, on_pane_notify, a);
  pane_set_cell_px(p, a->cell_w, a->cell_h);
  /* Every pane is made here, so this is the one place history has to be sized.
   * lib-vt's own default is 10,000 bytes -- 622 lines of an 80-column pane --
   * which is not a number anybody chose. */
  pane_set_scrollback(p, CFG.scrollback, CFG.scrollback_bytes);
  return n;
}

/* What a new pane runs when nobody said otherwise: whatever the session was
 * started with, else the config's `shell`, else $SHELL, else /bin/sh.
 *
 * Resolved per pane rather than once at startup, so editing `shell` and
 * saving affects the next pane you open instead of the next session you
 * start. The array is static because pane_new copies what it is given before
 * this could be called again. */
static const char *const *default_argv(app_t *a) {
  static const char *argv[2];
  if (a->argv && a->argv[0]) return a->argv;
  const char *sh = CFG.shell && *CFG.shell ? CFG.shell : getenv("SHELL");
  argv[0] = sh && *sh ? sh : "/bin/sh";
  argv[1] = NULL;
  return argv;
}

/* Where the program in a pane *is*, which after any amount of `cd` is not where
 * it was started. Defined with the dumper, declared here because a split wants
 * it too: a new pane opens where the one it came out of stands. */
static const char *live_cwd(const pane_t *p, char *buf, size_t cap);

/* A plain pane running the session's shell. One line, because everything a pane
 * needs setting up is in leaf_new_ex and a second copy of that list is a second
 * place to forget something -- which is exactly how panes came to have every
 * handler attached and no history limit. */
static node_t *leaf_new(app_t *a) {
  return leaf_new_ex(a, default_argv(a), NULL, false, "");
}

/* Empty a chain, freeing the programs it owned. */
static void chain_clear(inband_chain_t *c) {
  for (size_t i = 0; i < c->nexprs; i++) expr_free(c->exprs[i]);
  c->nexprs = 0;
  c->n = 0;
}

static void node_free(node_t *n) {
  if (!n) return;
  if (n->kind == NODE_LEAF) {
    chain_clear(&n->content_chain);
    chain_clear(&n->chrome_chain);
    pane_free(n->pane);
  } else {
    for (size_t i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    free(n->kids);
  }
  free(n);
}

/* Purposes are how tooling finds "the agent pane" (D8). Sanitised on ingest to
 * the charset sl0ppi settled on, so the format survives verbatim. */
static void sanitise_purpose(const char *in, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = in; *p && n + 1 < cap; p++) {
    char c = *p;
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' ||
              c == '/' || c == '-';
    if (ok) out[n++] = c;
  }
  out[n] = 0;
}

static tab_t *tab_add(app_t *a, const char *name) {
  if (a->ntabs == a->tabcap) {
    a->tabcap = a->tabcap ? a->tabcap * 2 : 4;
    a->tabs = realloc(a->tabs, a->tabcap * sizeof *a->tabs);
  }
  tab_t *t = &a->tabs[a->ntabs++];
  memset(t, 0, sizeof *t);
  t->id = ++a->next_tab_id;
  snprintf(t->name, sizeof t->name, "%s", name && *name ? name : "");
  return t;
}

app_t *app_new(const char *const argv[], uint16_t cols, uint16_t rows) {
  ensure_config();
  app_t *a = calloc(1, sizeof *a);
  a->argv = argv;
  a->cols = cols;
  a->rows = rows;
  /* A plausible cell until a client says otherwise, because zero is not a
   * neutral default here: it is the value that makes every naturally-sized
   * image cover no cells and quietly not appear. */
  a->cell_w = 8;
  a->cell_h = 16;
  a->gfx = gfx_new();
  tab_add(a, "");
  a->cur = 0;
  node_t *leaf = leaf_new(a);
  if (!leaf) {
    free(a->tabs);
    free(a);
    return NULL;
  }
  cur(a)->root = leaf;
  cur(a)->focus = leaf;
  return a;
}

void app_free(app_t *a) {
  if (!a) return;
  gfx_free(a->gfx);
  free(a->clipboard);
  free(a->clipboard_pending);
  free(a->projects);
  for (size_t i = 0; i < a->ntabs; i++) node_free(a->tabs[i].root);
  free(a->tabs);
  free(a);
}

bool app_should_quit(const app_t *a) { return a->quit || a->ntabs == 0; }
bool app_detach_requested(const app_t *a) { return a->detach; }
void app_clear_detach(app_t *a) { a->detach = false; }

/* ---- walking ------------------------------------------------------------ */

typedef void (*leaf_fn)(node_t *, void *);

static void walk(node_t *n, leaf_fn fn, void *ud) {
  if (!n) return;
  if (n->kind == NODE_LEAF) {
    fn(n, ud);
    return;
  }
  for (size_t i = 0; i < n->nkids; i++) walk(n->kids[i], fn, ud);
}

static void walk_all(app_t *a, leaf_fn fn, void *ud) {
  for (size_t i = 0; i < a->ntabs; i++) walk(a->tabs[i].root, fn, ud);
}

/* Which tab a node lives in: climb to its root and match. */
static size_t tab_of(app_t *a, node_t *n) {
  while (n->parent) n = n->parent;
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].root == n) return i;
  return (size_t)-1;
}

struct byid {
  uint32_t id;
  node_t *found;
};
static void byid_cb(node_t *n, void *ud) {
  struct byid *b = ud;
  if (n->id == b->id) b->found = n;
}

static void count_cb(node_t *n, void *ud) { (*(size_t *)ud)++; }

size_t app_pane_count(const app_t *a) {
  size_t n = 0;
  walk_all((app_t *)a, count_cb, &n);
  return n;
}

struct find_fd {
  int fd;
  node_t *found;
};
static void find_fd_cb(node_t *n, void *ud) {
  struct find_fd *f = ud;
  if (pane_fd(n->pane) == f->fd) f->found = n;
}

struct collect {
  int *out;
  size_t n, max;
};
/* Only panes that can still say something. A suspended pane has no pty yet
 * and a dead one no longer has one; handing either to poll() would be an fd
 * that is never readable, or — the expensive mistake — one at EOF that is
 * readable forever. */
static void collect_cb(node_t *n, void *ud) {
  struct collect *c = ud;
  if (pane_fd(n->pane) >= 0 && c->n < c->max) c->out[c->n++] = pane_fd(n->pane);
}

size_t app_fds(app_t *a, int *out, size_t max) {
  struct collect c = {out, 0, max};
  walk_all(a, collect_cb, &c); /* background tabs keep running */
  return c.n;
}

bool app_pump_fd(app_t *a, int fd) {
  struct find_fd f = {fd, NULL};
  walk_all(a, find_fd_cb, &f);
  if (!f.found) return false;
  /* Death is observed here, once, by the only call that can see the edge:
   * pane_pump() is where EOF arrives, and the note has to be written on the
   * transition or every later frame would write it again. */
  bool was_alive = pane_alive(f.found->pane);
  pane_pump(f.found->pane);
  if (was_alive && !pane_alive(f.found->pane)) {
    char words[64];
    exit_words(f.found->pane, words, sizeof words);
    /* What it ran, above how it ended. `[re-run]` is one button on a pane whose
     * program is gone, and a pane that does not say what it would run makes
     * pressing it a guess -- especially in a tab somebody else's layout built,
     * where the command was never typed here in the first place. A pane running
     * the session's shell has no command to name and gets the exit line alone.
     *
     * One note rather than two: `pane_note` puts a blank line either side of
     * what it writes, so two calls would space the two halves apart as if they
     * were about different things. */
    const char *label = pane_label(f.found->pane);
    char note[320];
    if (label && *label)
      snprintf(note, sizeof note, "[ran: %s]\r\n[process %s]", label, words);
    else
      snprintf(note, sizeof note, "[process %s]", words);
    pane_note(f.found->pane, note, DEAD_C);
  }
  return true;
}

/* ---- layout: a pure function of the tree and the rect ------------------- */

/* Every leaf under a node, in tree order. */
static size_t collect_leaves(node_t *n, node_t **out, size_t cap, size_t k) {
  if (!n || k >= cap) return k;
  if (n->kind == NODE_LEAF) {
    out[k++] = n;
    return k;
  }
  for (size_t i = 0; i < n->nkids; i++)
    k = collect_leaves(n->kids[i], out, cap, k);
  return k;
}

/* Does anything under here still want space in the layout? */
static bool subtree_live(node_t *n) {
  if (!n) return false;
  if (n->kind == NODE_LEAF) return !n->minimized;
  for (size_t i = 0; i < n->nkids; i++)
    if (subtree_live(n->kids[i])) return true;
  return false;
}

static size_t collect_minimized(node_t *n, node_t **out, size_t cap, size_t k) {
  if (!n || k >= cap) return k;
  if (n->kind == NODE_LEAF) {
    if (n->minimized) out[k++] = n;
    return k;
  }
  for (size_t i = 0; i < n->nkids; i++)
    k = collect_minimized(n->kids[i], out, cap, k);
  return k;
}

static size_t count_leaves(node_t *n) {
  if (!n) return 0;
  if (n->kind == NODE_LEAF) return 1;
  size_t k = 0;
  for (size_t i = 0; i < n->nkids; i++) k += count_leaves(n->kids[i]);
  return k;
}

/* A collapsed pane keeps running and keeps its size — it is simply not drawn,
 * and deliberately not resized, so nothing inside it reflows to one row and
 * back while you drag a terminal narrower. */
static void collapse_leaf(node_t *leaf, rect_t r) {
  leaf->collapsed = true;
  leaf->hidden = true;
  leaf->rect = r;
  /* `content` is deliberately left alone. It is the size the program inside
   * still believes it has — it was not resized on the way in and must not
   * appear to have been — and nothing draws a collapsed pane's content, so a
   * header rect there would only be a more convincing lie. */
}

/* The splits above a flattened stack span the whole area and draw nothing.
 * Their children no longer sit side by side, so the gap loop in draw_node
 * finds no gap between any two of them and registers no resize handles —
 * which is right, because there is nothing left to resize. */
static void span_splits(node_t *n, rect_t r) {
  if (!n || n->kind == NODE_LEAF) return;
  n->rect = r;
  n->collapsed = false;
  for (size_t i = 0; i < n->nkids; i++) span_splits(n->kids[i], r);
}

static node_t *first_leaf_of(node_t *n) {
  while (n && n->kind == NODE_SPLIT) n = n->kids[0];
  return n;
}

/* Laying out happens twice: once to ask whether the tab fits at all, and once
 * to do it. The probe must not resize any pane, because a pane it lays out
 * normally may be a header by the time the real pass is done, and a pane that
 * is about to be collapsed must not be told a size it will never show. */
typedef struct {
  node_t *focus;
  bool apply;    /* false during the probe */
  bool overflow; /* some node could not give its children their floor */
} layout_ctx_t;

static void layout_node(node_t *n, rect_t r, layout_ctx_t *ctx);

/* The collapse: the hierarchy stops existing for as long as it does not fit.
 *
 * Every pane under this node becomes one row in a flat list, with the focused
 * one opened below them. Nesting is exactly what there is no room to express,
 * so a subtree that cannot fit does not get to keep spending rows saying how
 * its panes are arranged — and a reader in this state wants "which pane",
 * not "which arrangement".
 *
 * Flat also makes every pane reachable. Collapsing a *subtree* to one header
 * gave every leaf inside it the same rect and one hit for the first of them,
 * so the rest could not be clicked at all. One row each fixes that by
 * construction.
 *
 * Headers first and the body below, rather than the open pane sitting in its
 * own position: switching then moves only the content, and the list you are
 * picking from holds still. The tree order it would have preserved is the
 * thing being flattened away anyway.
 *
 * Recomputed from the rect every frame, so there is no state to go stale —
 * which is the whole argument for D6 over swap layouts. */
static void layout_stack(node_t *n, rect_t r, layout_ctx_t *ctx) {
  node_t *leaves[64];
  size_t nl = collect_leaves(n, leaves, 64, 0);
  if (!nl) return;

  size_t expanded = 0;
  for (size_t i = 0; i < nl; i++)
    if (ctx->focus && leaves[i] == ctx->focus) expanded = i;

  span_splits(n, r);

  uint16_t headers = (uint16_t)(nl - 1);
  uint16_t body = r.h > headers ? (uint16_t)(r.h - headers) : 1;

  uint16_t y = r.y;
  for (size_t i = 0; i < nl; i++) {
    if (i == expanded) continue;
    collapse_leaf(leaves[i], (rect_t){r.x, y, r.w, 1});
    y = (uint16_t)(y + 1);
  }
  layout_node(leaves[expanded], (rect_t){r.x, y, r.w, body}, ctx);
}

/* Not even room for one row per pane plus something to look at: show the
 * focused pane and nothing else. The alternative is rects that do not fit on
 * the screen, and a pane one cell tall helps no one. */
static void layout_solo(node_t *n, rect_t r, layout_ctx_t *ctx) {
  node_t *leaves[64];
  size_t nl = collect_leaves(n, leaves, 64, 0);
  if (!nl) return;

  size_t expanded = 0;
  for (size_t i = 0; i < nl; i++)
    if (ctx->focus && leaves[i] == ctx->focus) expanded = i;

  span_splits(n, r);
  for (size_t i = 0; i < nl; i++)
    if (i != expanded) collapse_leaf(leaves[i], (rect_t){r.x, r.y, 0, 0});
  layout_node(leaves[expanded], r, ctx);
}

static void layout_node(node_t *n, rect_t r, layout_ctx_t *ctx) {
  /* Minimised panes are placed by the strip, not by the tree. Returning before
   * touching the rect leaves the strip's placement standing, and returning
   * before the floor check is right too: a pane that is one row by request is
   * not a pane that failed to fit. */
  if (n->kind == NODE_LEAF && n->minimized) return;

  n->rect = r;
  n->collapsed = false;
  if (n->kind == NODE_LEAF) n->hidden = false;
  if (n->kind == NODE_LEAF) {
    /* content is the frame deflated by its border and its padding, per side; a
     * rect too small for a frame gets neither, and the pane takes the whole
     * thing. Horizontal padding is aspect-corrected, like the gap: the config's
     * numbers are rows, so a square-looking ring is one number rather than an
     * arithmetic problem. */
    bool framed = r.w >= 3 && r.h >= 3;
    uint16_t left = framed ? 1 + CFG.pad_left * CFG.gap_aspect : 0;
    uint16_t right = framed ? 1 + CFG.pad_right * CFG.gap_aspect : 0;
    uint16_t top = framed ? 1 + CFG.pad_top : 0;
    uint16_t bottom = framed ? 1 + CFG.pad_bottom : 0;
    n->content = (rect_t){
        .x = (uint16_t)(r.x + left),
        .y = (uint16_t)(r.y + top),
        .w = (uint16_t)(r.w > left + right ? r.w - left - right : 1),
        .h = (uint16_t)(r.h > top + bottom ? r.h - top - bottom : 1),
    };
    if (ctx->apply && !n->hidden)
      pane_resize(n->pane, n->content.w, n->content.h);

    /* A pane has to clear the floor in *both* directions, whatever arrangement
     * produced it. The check on a split node only ever asks about the
     * dimension that node divides — a row split asks about height and never
     * about width — so a column of stacked panes could be squeezed to four
     * cells wide with nothing in the tree responsible for noticing. The leaf
     * is the only place that can see both of its own sides. */
    if (r.w < MIN_PANE_COLS || r.h < MIN_PANE_ROWS) ctx->overflow = true;
    return;
  }

  /* Divide among the children that still want space. A split with one live
   * child is that child, exactly as a split with one child would be. */
  node_t *live[64];
  size_t k = 0;
  for (size_t i = 0; i < n->nkids && k < 64; i++)
    if (subtree_live(n->kids[i])) live[k++] = n->kids[i];
  if (k == 0) return;
  if (k == 1) {
    layout_node(live[0], r, ctx);
    return;
  }

  uint16_t gap =
      n->dir == SPLIT_COLS ? (uint16_t)(CFG.gap * CFG.gap_aspect) : CFG.gap;
  uint16_t total = n->dir == SPLIT_COLS ? r.w : r.h;

  /* Does every child clear the floor? If not, this node collapses — a local
   * decision, made from this rect, affecting nothing above or below it. */
  uint16_t floor_ = n->dir == SPLIT_COLS ? MIN_PANE_COLS : MIN_PANE_ROWS;
  uint16_t need = (uint16_t)(k * floor_ + gap * (k - 1));
  if (total < need) {
    /* Report and stop. Whether this becomes a stack is not this node's call
     * any more: a tab is either laid out or it is a list, and half a screen of
     * each is the state nobody wants to be looking at. layout() decides, once,
     * for the whole tab. */
    ctx->overflow = true;
    return;
  }

  uint16_t gaps = (uint16_t)(gap * (k - 1));
  uint16_t avail = total > gaps ? (uint16_t)(total - gaps) : (uint16_t)k;
  if (avail / k == 0) { /* below one cell each, nothing sensible is left */
    ctx->overflow = true;
    return;
  }

  /* Sizes are proportional to weights; the remainder goes to the widest
   * children, largest first, so nothing drifts and nothing rounds to zero. */
  long total_weight = 0;
  for (size_t i = 0; i < k; i++) total_weight += live[i]->weight;
  if (total_weight <= 0) total_weight = 1;

  uint16_t sizes[64];
  uint16_t used = 0;
  for (size_t i = 0; i < k && i < 64; i++) {
    long want = (long)avail * live[i]->weight / total_weight;
    if (want < 1) want = 1;
    sizes[i] = (uint16_t)want;
    used = (uint16_t)(used + sizes[i]);
  }
  for (size_t i = 0; used < avail && i < k; i = (i + 1) % k) {
    sizes[i]++;
    used++;
  }
  for (size_t i = 0; used > avail && i < k; i = (i + 1) % k) {
    if (sizes[i] > 1) {
      sizes[i]--;
      used--;
    }
  }

  uint16_t pos = n->dir == SPLIT_COLS ? r.x : r.y;
  for (size_t i = 0; i < k; i++) {
    uint16_t size = sizes[i];
    rect_t cr = n->dir == SPLIT_COLS
                    ? (rect_t){.x = pos, .y = r.y, .w = size, .h = r.h}
                    : (rect_t){.x = r.x, .y = pos, .w = r.w, .h = size};
    layout_node(live[i], cr, ctx);
    pos = (uint16_t)(pos + size + gap);
  }
}

#define STRIP_ROWS (CFG.status_bar ? 1 : 0)
#define LINE_ROWS (CFG.status_line ? 1 : 0)

static node_t *pane_by_id(app_t *a, uint32_t id); /* defined below */
static size_t tab_of(app_t *a, node_t *n);

static void layout(app_t *a) {
  if (!a->ntabs) return;
  /* Re-derived below, like everything else about a layout pass: whether this
   * tab is a layout or a list (D6). Recorded because a tree edit needs to be
   * able to ask what its edit did — a turn that flattens the tab is a turn
   * nobody asked for. */
  a->flattened = false;
  uint16_t gx = (uint16_t)(CFG.gap * CFG.gap_aspect), gy = CFG.gap;
  uint16_t top = (uint16_t)(gy + STRIP_ROWS);
  rect_t r = {.x = gx,
              .y = top,
              .w = (uint16_t)(a->cols > 2 * gx ? a->cols - 2 * gx : a->cols),
              .h = (uint16_t)(a->rows > top + gy + LINE_ROWS
                                  ? a->rows - top - gy - LINE_ROWS
                                  : 1)};
  node_t *root = cur(a)->root;
  if (!root) return;

  /* Focusing a minimised pane is how you get it back, and it is checked here
   * rather than in the several places focus can move: any route that ends with
   * this pane focused restores it, including ones that do not exist yet. */
  if (cur(a)->focus && cur(a)->focus->minimized)
    cur(a)->focus->minimized = false;

  /* A zoomed pane is the whole tab. Reuses the solo path rather than adding a
   * third arrangement: "show this one and nothing else" is a thing the layout
   * already knew how to do, it just used to be a last resort rather than a
   * request. The id is validated here so a zoom cannot outlive its pane. */
  if (cur(a)->zoom) {
    node_t *z = pane_by_id(a, cur(a)->zoom);
    if (!z || tab_of(a, z) != a->cur) {
      cur(a)->zoom = 0;
    } else {
      layout_ctx_t only = {.focus = z, .apply = true};
      layout_solo(root, r, &only);
      return;
    }
  }

  /* Ask first, then do it. A tab is either laid out or it is a list of panes;
   * there is no third state where one half of the screen is a stack and the
   * other half is not, because that is the arrangement that explains neither
   * what happened nor what to do about it.
   *
   * So the question is asked of the whole tab: if *any* node cannot give its
   * children their floor, the entire tab flattens. The probe is the real
   * layout with resizing switched off, rather than a second implementation of
   * the same arithmetic — two copies of "does this fit" is exactly how the
   * guide and the layout would start disagreeing. */
  /* Minimised panes come out of the layout and sit in a strip along the
   * bottom, one row each. The tree is then laid out in what is left, so a
   * minimised pane costs a row rather than a share. */
  /* Minimised panes are listed on one row along the bottom, however many there
   * are. A row each would let putting things away cost more room than having
   * them out, which is the opposite of the point. */
  node_t *mins[64];
  size_t nmin = collect_minimized(root, mins, 64, 0);
  rect_t tree_r = r;
  bool no_room = false;
  cur(a)->min_bar = (rect_t){0, 0, 0, 0};
  if (nmin) {
    if (r.h >= (uint16_t)(MIN_PANE_ROWS + 3)) {
      tree_r.h = (uint16_t)(r.h - 1);
      cur(a)->min_bar = (rect_t){r.x, (uint16_t)(r.y + r.h - 1), r.w, 1};
    } else {
      no_room = true;
    }
  }

  layout_ctx_t probe = {.focus = cur(a)->focus, .apply = false};
  layout_node(root, tree_r, &probe);
  /* Nowhere to put the bar means nowhere to put anything: flatten, where every
   * pane is a row and being minimised is not a different thing to be. */
  if (no_room) probe.overflow = true;

  layout_ctx_t real = {.focus = cur(a)->focus, .apply = true};
  if (!probe.overflow) {
    layout_node(root, tree_r, &real);
    /* They are drawn by the bar, not as panes: no rect of their own. */
    for (size_t i = 0; i < nmin; i++)
      collapse_leaf(mins[i], (rect_t){r.x, r.y, 0, 0});
    return;
  }
  a->flattened = true;
  cur(a)->min_bar = (rect_t){0, 0, 0, 0}; /* a list has no separate bar */
  if (r.h >= (uint16_t)(count_leaves(root) + 2)) {
    /* One row per pane, plus a body worth opening into: a frame is two rows. */
    layout_stack(root, r, &real);
  } else {
    layout_solo(root, r, &real);
  }
}

void app_write_focused(app_t *a, const void *buf, size_t len) {
  if (cur(a)->focus) pane_write(cur(a)->focus->pane, buf, len);
}

void app_resize(app_t *a, uint16_t cols, uint16_t rows) {
  a->cols = cols;
  a->rows = rows;
  layout(a);
}

static void set_cell_px_cb(node_t *n, void *ud) {
  const uint16_t *px = ud;
  pane_set_cell_px(n->pane, px[0], px[1]);
}

void app_set_cell_px(app_t *a, uint16_t w, uint16_t h) {
  if (!w || !h || (w == a->cell_w && h == a->cell_h)) return;
  a->cell_w = w;
  a->cell_h = h;
  uint16_t px[2] = {w, h};
  /* Every pane, not just the visible ones: a background tab's program may be
   * drawing images too, and it should not have to be looked at to find out
   * how big a cell is. */
  walk_all(a, set_cell_px_cb, px);
}

void app_cell_px(const app_t *a, uint16_t *w, uint16_t *h) {
  if (w) *w = a->cell_w;
  if (h) *h = a->cell_h;
}

/* ---- tree edits --------------------------------------------------------- */

static void replace_child(node_t *parent, node_t *old, node_t *new_) {
  for (size_t i = 0; i < parent->nkids; i++)
    if (parent->kids[i] == old) {
      parent->kids[i] = new_;
      new_->parent = parent;
      return;
    }
}

/* The pane a split makes. NULL argv means "whatever this session runs", which
 * is the ordinary case; a command makes it a task pane instead. */
static node_t *split_node_with(app_t *a, node_t *leaf, split_dir_t dir,
                               bool before, const char *const *argv,
                               const char *label);

static void split_node(app_t *a, node_t *leaf, split_dir_t dir, bool before) {
  split_node_with(a, leaf, dir, before, NULL, NULL);
}

/* Put `node` beside `leaf`, splitting `dir`, on the side `before` names.
 *
 * The tree surgery of a split without the pane: `split_node_with` hands it a leaf
 * it just made, and moving a pane between tabs hands it one that already exists
 * somewhere else. `t` is the tab `leaf` lives in, because replacing a root has to
 * land on the right one -- `cur(a)` was right only while the only caller was
 * splitting the pane you were looking at. */
static void place_beside(app_t *a, tab_t *t, node_t *leaf, node_t *node,
                         split_dir_t dir, bool before) {
  /* Growing an existing split in the same direction keeps the tree flat, so
   * three vertical splits are three equal columns rather than 1/2 + 1/4 + 1/4. */
  if (leaf->parent && leaf->parent->dir == dir) {
    node_t *p = leaf->parent;
    p->kids = realloc(p->kids, (p->nkids + 1) * sizeof *p->kids);
    size_t at = 0;
    while (at < p->nkids && p->kids[at] != leaf) at++;
    size_t slot = before ? at : at + 1;
    memmove(&p->kids[slot + 1], &p->kids[slot],
            (p->nkids - slot) * sizeof *p->kids);
    p->kids[slot] = node;
    node->parent = p;
    p->nkids++;
  } else {
    node_t *sp = calloc(1, sizeof *sp);
    sp->kind = NODE_SPLIT;
    sp->id = ++a->next_id;
    sp->weight = leaf->weight; /* the new split inherits the pane's share */
    leaf->weight = WEIGHT_UNIT;
    node->weight = WEIGHT_UNIT;
    sp->dir = dir;
    sp->nkids = 2;
    sp->kids = malloc(2 * sizeof *sp->kids);
    sp->kids[before ? 1 : 0] = leaf;
    sp->kids[before ? 0 : 1] = node;
    sp->parent = leaf->parent;
    if (leaf->parent)
      replace_child(leaf->parent, leaf, sp);
    else
      t->root = sp;
    leaf->parent = sp;
    node->parent = sp;
  }
}

static node_t *split_node_with(app_t *a, node_t *leaf, split_dir_t dir,
                               bool before, const char *const *argv,
                               const char *label) {
  if (!leaf) return NULL;
  /* A new pane opens where the pane it came out of *is*, not where the session
   * was started. Splitting inside a project and landing in whatever directory
   * the server happened to be launched from is wrong on its own, and it made a
   * saved project layout carry an absolute path to somewhere else entirely. */
  char cwdbuf[4096];
  const char *cwd = live_cwd(leaf->pane, cwdbuf, sizeof cwdbuf);
  node_t *fresh = argv ? leaf_new_ex(a, argv, cwd, false, label)
                       : leaf_new_ex(a, default_argv(a), cwd, false, "");
  if (!fresh) return NULL;

  place_beside(a, cur(a), leaf, fresh, dir, before);
  cur(a)->focus = fresh;
  layout(a);
  return fresh;
}

/* Would splitting this pane produce two panes worth having?
 *
 * This is the layout's own arithmetic, not an approximation of it: the same
 * floor, the same gap, and the same child count the split would actually
 * create — which is one more sibling when the parent already splits this way,
 * and two when it does not. Guessing here would let the guide promise a split
 * that immediately collapsed, and the guide's whole claim is that drawing and
 * the layout cannot disagree. */
static bool split_fits(node_t *leaf, split_dir_t dir) {
  /* Two different questions, and the larger answer wins: min_pane is where the
   * layout gives up, min_split is where splitting stopped being worth
   * offering. A pane can clear the first and still be too small to be worth
   * halving — which is the whole point of having both. */
  uint16_t floor_ = dir == SPLIT_COLS ? CFG.min_split_cols : CFG.min_split_rows;
  uint16_t hard = dir == SPLIT_COLS ? MIN_PANE_COLS : MIN_PANE_ROWS;
  if (floor_ < hard) floor_ = hard;
  uint16_t gap =
      dir == SPLIT_COLS ? (uint16_t)(CFG.gap * CFG.gap_aspect) : CFG.gap;

  size_t k;
  uint16_t total;
  if (leaf->parent && leaf->parent->dir == dir) {
    k = leaf->parent->nkids + 1;
    total = dir == SPLIT_COLS ? leaf->parent->rect.w : leaf->parent->rect.h;
  } else {
    k = 2;
    total = dir == SPLIT_COLS ? leaf->rect.w : leaf->rect.h;
  }
  if (total < (uint16_t)(k * floor_ + gap * (k - 1))) return false;

  /* And the axis this split does *not* divide has to already clear the floor,
   * because splitting will not improve it. A pane four rows tall on a short
   * terminal has room for two columns by every measure this function used to take,
   * and produces two panes the layout immediately collapses into a list -- so the
   * border was offering, and the keys were doing, something that could only undo
   * the arrangement. The same hole `layout_node` closed at the leaf: the check on a
   * split only ever asked about the dimension that split divides. */
  uint16_t across = dir == SPLIT_COLS ? leaf->rect.h : leaf->rect.w;
  uint16_t across_floor = dir == SPLIT_COLS ? MIN_PANE_ROWS : MIN_PANE_COLS;
  return across >= across_floor;
}

static split_dir_t side_dir(char side) {
  return (side == 'l' || side == 'r') ? SPLIT_COLS : SPLIT_ROWS;
}

static void split_focus(app_t *a, split_dir_t dir) {
  split_node(a, cur(a)->focus, dir, false);
}

/* Splitting because a person just asked for it, which is the only case the
 * floor applies to.
 *
 * The keyboard obeys the same floor the border does: a rule only the mouse
 * followed would mean the same session splits or refuses depending on how you
 * asked, and whether there is room cannot depend on that. The control API and
 * layout files go through split_focus() and are deliberately *not* bound by
 * it — a script asking for a pane is declaring what it wants, not being
 * offered something, and D6's responsive collapse is what catches it there. */
static void split_focus_ui(app_t *a, split_dir_t dir) {
  node_t *n = cur(a)->focus;
  if (!n) return;
  if (!split_fits(n, dir)) {
    app_toast(a, dir == SPLIT_COLS ? "no room to split across"
                                   : "no room to split down");
    return;
  }
  split_node(a, n, dir, false);
}

/* Which way this pane wants to be cut: across its longer side, so the two halves
 * come out as square as the pane allows.
 *
 * "Longer" is a question about what you can *see*, and a cell is about twice as
 * tall as it is wide -- so 80x24 is wider than it is tall (80 against 48) while
 * 40x24 is not (40 against 48). `gap_aspect` is already this program's answer to
 * how many columns a row is worth, so it is the number used here rather than a
 * second constant meaning the same thing: a config that says cells are square gets
 * a square answer. */
static split_dir_t preferred_dir(const node_t *leaf) {
  uint16_t aspect = CFG.gap_aspect ? CFG.gap_aspect : 1;
  return (uint32_t)leaf->rect.w >= (uint32_t)leaf->rect.h * aspect ? SPLIT_COLS
                                                                   : SPLIT_ROWS;
}

/* Split whichever way there is room for, preferring the longer side.
 *
 * The fallback is the point: a pane that is wide but six rows tall wants cutting
 * across, and if that will not fit `split_fits` says so and the other axis is tried
 * before giving up. Refusing while a split was available would be the worse answer.
 * It does not pick quietly either -- one key that can do two things has to say
 * which it did. */
static void split_focus_auto(app_t *a) {
  node_t *n = cur(a)->focus;
  if (!n) return;
  split_dir_t want = preferred_dir(n);
  split_dir_t other = want == SPLIT_COLS ? SPLIT_ROWS : SPLIT_COLS;
  split_dir_t dir;
  if (split_fits(n, want))
    dir = want;
  else if (split_fits(n, other))
    dir = other;
  else {
    app_toast(a, "no room to split");
    return;
  }
  split_node(a, n, dir, false);
  /* The same two words the border click uses for the same two outcomes. */
  app_toast(a, dir == SPLIT_COLS ? "split right" : "split down");
}

static node_t *first_leaf(node_t *n) {
  while (n && n->kind == NODE_SPLIT) n = n->kids[0];
  return n;
}

static void tab_remove(app_t *a, size_t ti) {
  node_free(a->tabs[ti].root);
  memmove(&a->tabs[ti], &a->tabs[ti + 1],
          (a->ntabs - ti - 1) * sizeof *a->tabs);
  a->ntabs--;
  if (a->cur >= a->ntabs && a->ntabs) a->cur = a->ntabs - 1;
}

static void close_leaf(app_t *a, node_t *leaf) {
  size_t ti = tab_of(a, leaf);
  if (ti == (size_t)-1) return;
  tab_t *t = &a->tabs[ti];
  node_t *p = leaf->parent;

  if (!p) { /* the tab's last pane: the tab goes with it */
    t->root = NULL;
    node_free(leaf);
    tab_remove(a, ti);
    if (a->ntabs == 0) a->quit = true;
    return;
  }

  size_t at = 0;
  while (at < p->nkids && p->kids[at] != leaf) at++;
  memmove(&p->kids[at], &p->kids[at + 1],
          (p->nkids - at - 1) * sizeof *p->kids);
  p->nkids--;
  node_free(leaf);

  node_t *survivor = p->kids[0];
  if (p->nkids == 1) { /* a split with one child is just that child */
    survivor->parent = p->parent;
    if (p->parent)
      replace_child(p->parent, p, survivor);
    else
      t->root = survivor;
    free(p->kids);
    free(p);
  }

  if (t->focus == leaf || t->focus == p) t->focus = first_leaf(survivor);
  layout(a);
}

struct reap {
  app_t *a;
  node_t *dead;
};
/* Does this pane's corpse stay? The label is the command it was given, and is
 * empty for a pane that is just a shell -- which is the whole distinction, and
 * it happens to already be recorded. */
static bool keep_corpse(const pane_t *p) {
  /* A pane opened to do one thing is done when that thing ends, whatever the
   * policy says about commands: you asked for an editor, not for a record of
   * having had one. */
  if (pane_ephemeral(p)) return false;
  switch (CFG.keep_dead) {
  case KEEP_DEAD_ALL: return true;
  case KEEP_DEAD_NONE: return false;
  default: return pane_label(p)[0] != 0;
  }
}

static void reap_cb(node_t *n, void *ud) {
  struct reap *r = ud;
  if (!r->dead && !pane_alive(n->pane) && !keep_corpse(n->pane)) r->dead = n;
}

/* Closing a pane whose program exited is a *policy*, not bookkeeping.
 *
 * A pane that was given a command keeps its corpse: the error is worth
 * reading and [re-run] is worth having. A pane that is a shell goes when you
 * type `exit`, because that is what typing `exit` means. `keep_dead` moves
 * the line either way. */
void app_reap(app_t *a) {
  ensure_config();
  for (;;) {
    struct reap r = {a, NULL};
    walk_all(a, reap_cb, &r);
    if (!r.dead) break;
    close_leaf(a, r.dead);
    if (a->ntabs == 0) break;
  }
}

uint32_t app_new_tab(app_t *a, const char *name) {
  tab_t *t = tab_add(a, name);
  node_t *leaf = leaf_new(a);
  if (!leaf) {
    a->ntabs--;
    return 0;
  }
  t->root = leaf;
  t->focus = leaf;
  a->cur = a->ntabs - 1;
  layout(a);
  return t->id;
}

bool app_select_tab(app_t *a, size_t index) {
  if (index >= a->ntabs) return false;
  a->cur = index;
  layout(a);
  return true;
}

bool app_select_tab_id(app_t *a, uint32_t id) {
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].id == id) return app_select_tab(a, i);
  return false;
}

void app_cycle_tab(app_t *a, int delta) {
  if (a->ntabs < 2) return;
  long n = (long)a->ntabs;
  a->cur = (size_t)(((long)a->cur + delta % n + n) % n);
  layout(a);
}

size_t app_tab_count(const app_t *a) { return a->ntabs; }

bool app_close_tab(app_t *a, uint32_t id) {
  for (size_t i = 0; i < a->ntabs; i++) {
    if (a->tabs[i].id != id) continue;
    tab_remove(a, i);
    if (a->ntabs == 0)
      a->quit = true;
    else
      layout(a);
    return true;
  }
  return false;
}

/* D8's trust model: a purpose declared by a layout outranks an in-band one and
 * cannot be overridden, so `cat hostile.txt` in a pane cannot relabel a
 * project tab. `declared` is only ever true on the control path.
 *
 * Clearing a declared purpose *unlocks* it. Setting it to nothing means "this
 * pane has no purpose of mine", and a lock held over an empty string would be
 * the one state nobody can get out of -- the program can no longer label
 * itself and there is nothing there to have outranked it. Same shape as
 * clearing a pane's name, which hands the title back to the program. */
static void set_purpose(char *slot, size_t cap, bool *locked,
                        const char *purpose, bool declared) {
  sanitise_purpose(purpose, slot, cap);
  if (declared) *locked = slot[0] != 0;
}

/* `id` 0 is the focused pane, and the tab you are looking at, the way it is
 * everywhere else a verb takes one (`close`, `rerun`, `clear-shaders`). It was
 * the one addressable thing that refused it, which read as "that purpose is
 * not allowed" rather than "say which pane". */
bool app_set_pane_purpose(app_t *a, uint32_t id, const char *purpose,
                          bool declared) {
  node_t *found = NULL;
  if (!id) {
    found = cur(a)->focus;
  } else {
    struct byid b = {id, NULL};
    walk_all(a, byid_cb, &b);
    found = b.found;
  }
  if (!found) return false;
  if (found->purpose_locked && !declared) return false;
  set_purpose(found->purpose, sizeof found->purpose, &found->purpose_locked,
              purpose, declared);
  return true;
}

bool app_set_tab_purpose(app_t *a, uint32_t id, const char *purpose,
                         bool declared) {
  tab_t *t = id ? NULL : cur(a);
  for (size_t i = 0; !t && i < a->ntabs; i++)
    if (a->tabs[i].id == id) t = &a->tabs[i];
  if (!t) return false;
  if (t->purpose_locked && !declared) return false;
  set_purpose(t->purpose, sizeof t->purpose, &t->purpose_locked, purpose,
              declared);
  return true;
}

static tab_t *tab_by_id(app_t *a, uint32_t id) {
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].id == id) return &a->tabs[i];
  return NULL;
}

static size_t tab_index(app_t *a, uint32_t id) {
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].id == id) return i;
  return (size_t)-1;
}

/* Move a tab to another position in the strip.
 *
 * A list operation on a list: the strip *is* the order, so there is no
 * separate ordering to keep in sync with it. `cur` is a position rather than
 * an identity, so it has to be recomputed here — carrying it across a move
 * would leave you looking at whichever tab slid into the index you used to
 * be at, which is the kind of bug that only shows up in front of someone. */
static void move_tab(app_t *a, size_t from, size_t to) {
  if (from == to || from >= a->ntabs || to >= a->ntabs) return;
  tab_t moved = a->tabs[from];
  if (from < to)
    memmove(&a->tabs[from], &a->tabs[from + 1], (to - from) * sizeof *a->tabs);
  else
    memmove(&a->tabs[to + 1], &a->tabs[to], (from - to) * sizeof *a->tabs);
  a->tabs[to] = moved;

  if (a->cur == from)
    a->cur = to;
  else if (a->cur > from && a->cur <= to)
    a->cur--;
  else if (a->cur >= to && a->cur < from)
    a->cur++;
}

bool app_move_tab(app_t *a, uint32_t id, size_t index) {
  size_t from = tab_index(a, id);
  if (from == (size_t)-1 || index >= a->ntabs) return false;
  move_tab(a, from, index);
  return true;
}

bool app_set_tab_name(app_t *a, uint32_t id, const char *name) {
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].id == id) {
      snprintf(a->tabs[i].name, sizeof a->tabs[i].name, "%s", name);
      return true;
    }
  return false;
}

/* Control-API verbs that address a pane by id, rather than "the focused one".
 * They select the pane's tab first, so scripting a background tab works. */
static node_t *pane_by_id(app_t *a, uint32_t id) {
  struct byid b = {id, NULL};
  walk_all(a, byid_cb, &b);
  return b.found;
}

/* Naming a pane, for the double-click gesture and for a script: 0 is the focused
 * one, and an empty name clears it.
 *
 * This is the whole answer to a program that keeps setting a title it stopped
 * meaning -- an agent still spinning the summary of a task it finished, say.
 * `pane_title` prefers a name only while there is one, so what a person or a
 * script wrote outranks the program's own until they hand it back. Naming does
 * not select the pane's tab, unlike the verbs that rearrange things: a label is
 * not a reason to move what somebody is looking at. */
bool app_set_pane_name(app_t *a, uint32_t id, const char *name) {
  node_t *n = id ? pane_by_id(a, id) : cur(a)->focus;
  if (!n) return false;
  pane_set_name(n->pane, name);
  return true;
}

bool app_minimize(app_t *a, uint32_t id) {
  node_t *n = id ? pane_by_id(a, id) : cur(a)->focus;
  if (!n || n->minimized) return false;
  size_t ti = tab_of(a, n);
  if (ti == (size_t)-1) return false;

  /* Refused if it would leave nothing on screen. A tab showing no panes at all
   * has no way back that is discoverable from the tab, and "everything is put
   * away" is not a state worth being able to reach by accident. */
  node_t *next = NULL;
  node_t *leaves[64];
  size_t nl = collect_leaves(a->tabs[ti].root, leaves, 64, 0);
  for (size_t i = 0; i < nl; i++)
    if (leaves[i] != n && !leaves[i]->minimized) {
      next = leaves[i];
      break;
    }
  if (!next) return false;

  n->minimized = true;
  a->cur = ti;
  /* Focus has to leave, or the rule that a focused pane is never minimised
   * would undo this on the next frame. */
  if (a->tabs[ti].focus == n) a->tabs[ti].focus = next;
  layout(a);
  return true;
}

bool app_toggle_zoom(app_t *a, uint32_t id) {
  node_t *n = id ? pane_by_id(a, id) : cur(a)->focus;
  if (!n) return false;
  size_t ti = tab_of(a, n);
  if (ti == (size_t)-1) return false;
  a->cur = ti;
  /* Zooming focuses what it zoomed: the pane filling the tab is the only one
   * you could be typing into, and leaving focus elsewhere would mean keys
   * going somewhere invisible. */
  a->tabs[ti].zoom = a->tabs[ti].zoom == n->id ? 0 : n->id;
  a->tabs[ti].focus = n;
  layout(a);
  return true;
}

bool app_pane_zoomed(app_t *a, uint32_t id) {
  return a->ntabs && cur(a)->zoom == id && id != 0;
}

void app_set_session(app_t *a, const char *name) {
  snprintf(a->session, sizeof a->session, "%s", name ? name : "");
}

bool app_focus_pane(app_t *a, uint32_t id) {
  node_t *n = pane_by_id(a, id);
  if (!n) return false;
  size_t ti = tab_of(a, n);
  if (ti == (size_t)-1) return false;
  a->cur = ti;
  a->tabs[ti].focus = n;
  layout(a);
  return true;
}

bool app_split_pane(app_t *a, uint32_t id, bool rows) {
  if (id && !app_focus_pane(a, id)) return false;
  split_focus(a, rows ? SPLIT_ROWS : SPLIT_COLS);
  return true;
}

/* Take a leaf out of its tree and leave it whole: the surgery `close_leaf` does,
 * without the funeral. The pane keeps running the whole time -- nothing is
 * re-spawned by a move, which is the entire point of moving it rather than opening
 * one somewhere else and closing this.
 *
 * `*emptied` says the tab has nothing left in it. Removing it is the caller's, not
 * because that is tidier but because removing a tab shifts every index after it,
 * and a caller holding one has to know. */
static bool detach_leaf(app_t *a, node_t *leaf, size_t *from, bool *emptied) {
  size_t ti = tab_of(a, leaf);
  if (ti == (size_t)-1) return false;
  tab_t *t = &a->tabs[ti];
  *from = ti;
  *emptied = false;

  /* Nothing about this pane should still say where it used to be. A zoom is the
   * tab's opinion about one of its panes, and a minimised pane is one this tab put
   * away; carried across, the first would zoom a pane that has left and the second
   * would land the arrival in a strip nobody asked for. */
  if (t->zoom == leaf->id) t->zoom = 0;
  leaf->minimized = false;

  node_t *p = leaf->parent;
  if (!p) {
    t->root = NULL;
    t->focus = NULL;
    *emptied = true;
    return true;
  }

  size_t at = 0;
  while (at < p->nkids && p->kids[at] != leaf) at++;
  memmove(&p->kids[at], &p->kids[at + 1],
          (p->nkids - at - 1) * sizeof *p->kids);
  p->nkids--;
  leaf->parent = NULL;

  node_t *survivor = p->kids[0];
  if (p->nkids == 1) { /* a split with one child is just that child */
    survivor->parent = p->parent;
    if (p->parent)
      replace_child(p->parent, p, survivor);
    else
      t->root = survivor;
    free(p->kids);
    free(p);
  }
  if (t->focus == leaf || t->focus == p) t->focus = first_leaf(survivor);
  return true;
}

/* Move a pane into another tab, beside whatever that tab has focused.
 *
 * 0 means the focused pane, as everywhere. The destination is named by *id* rather
 * than index on purpose: emptying the source tab removes it, and every index after
 * it moves -- an id survives that and an index quietly means a different tab.
 *
 * False when there is nothing to do or nowhere to do it: no such pane, no such tab,
 * or the pane is already in that tab. The tab you are looking at does not change;
 * whether a caller follows the pane is a question about intent, and this is the
 * mechanism. */
bool app_move_pane_to_tab(app_t *a, uint32_t pane_id, uint32_t tab_id,
                          bool rows) {
  node_t *leaf = pane_id ? pane_by_id(a, pane_id) : cur(a)->focus;
  if (!leaf || leaf->kind != NODE_LEAF) return false;
  tab_t *dest = tab_by_id(a, tab_id);
  if (!dest || !dest->root) return false;
  size_t src_ti = tab_of(a, leaf);
  if (src_ti == (size_t)-1 || &a->tabs[src_ti] == dest) return false;

  uint32_t dest_id = dest->id, cur_id = a->tabs[a->cur].id;
  size_t from;
  bool emptied = false;
  if (!detach_leaf(a, leaf, &from, &emptied)) return false;
  if (emptied) tab_remove(a, from);

  /* Re-found by id, because the removal above may have moved it. */
  dest = tab_by_id(a, dest_id);
  if (!dest) { /* cannot happen: the destination is not the tab we emptied */
    node_free(leaf);
    return false;
  }
  node_t *beside = dest->focus ? dest->focus : first_leaf(dest->root);
  place_beside(a, dest, beside, leaf, rows ? SPLIT_ROWS : SPLIT_COLS, false);
  dest->focus = leaf;
  /* A zoomed destination would hide the arrival behind the pane filling it, which
   * looks exactly like the move having failed. */
  dest->zoom = 0;

  /* Stay where we were looking, unless that tab is the one that just went. */
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].id == cur_id) a->cur = i;
  layout(a);
  return true;
}

/* The same, into a tab of its own. Returns the new tab's id, or 0.
 *
 * Worth having as its own call rather than "move to a tab you make first": a pane
 * that is the only thing in its tab would otherwise have its tab removed from under
 * the new one, and the order in which those two happen is exactly the bug. */
uint32_t app_move_pane_to_new_tab(app_t *a, uint32_t pane_id,
                                  const char *name) {
  node_t *leaf = pane_id ? pane_by_id(a, pane_id) : cur(a)->focus;
  if (!leaf || leaf->kind != NODE_LEAF) return 0;
  size_t src_ti = tab_of(a, leaf);
  if (src_ti == (size_t)-1) return 0;
  /* A pane alone in its tab is already in a tab of its own. */
  if (!leaf->parent) return 0;

  uint32_t cur_id = a->tabs[a->cur].id;
  size_t from;
  bool emptied = false;
  if (!detach_leaf(a, leaf, &from, &emptied)) return 0;
  if (emptied) tab_remove(a, from);

  tab_t *t = tab_add(a, name);
  t->root = leaf;
  t->focus = leaf;
  leaf->parent = NULL;
  leaf->weight = WEIGHT_UNIT;
  uint32_t id = t->id;

  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].id == cur_id) a->cur = i;
  layout(a);
  return id;
}

bool app_close_pane(app_t *a, uint32_t id) {
  node_t *n = id ? pane_by_id(a, id) : cur(a)->focus;
  if (!n) return false;
  close_leaf(a, n);
  return true;
}

/* Run a dead pane's command again, in the pane it died in.
 *
 * The pane is the same pane throughout — same id, same place in the tree,
 * same terminal — so nothing that referred to it has to be told anything, and
 * the run that ended stays above the new one in its scrollback. A suspended
 * pane is started rather than restarted, because "run the thing this pane is
 * for" is the same request either way. */
/* Open the config in $EDITOR, in a pane of its own.
 *
 * The whole point is the loop: edit, save, watch the session change under you,
 * edit again. Anything that makes you leave the session to do it -- another
 * terminal, or remembering where the file lives -- is friction in the middle
 * of the one workflow the config watcher exists for.
 *
 * The pane is ephemeral: quit the editor and it is gone. You asked for an
 * editor, not for a record of having had one. */
bool app_edit_config(app_t *a) {
  node_t *n = a->ntabs ? cur(a)->focus : NULL;
  if (!n) return false;

  const char *path = config_default_path();

  /* An editor opened on a file that is not there is a blank buffer, and a
   * blank buffer does not tell you what you can set. So write the defaults
   * out first: every knob with its default, generated from the code rather
   * than from a copy of it. Only when there is nothing there --
   * this must never touch a config somebody has written. */
  if (access(path, F_OK) != 0) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
      *slash = 0;
      path_mkdirs(dir);
    }
    FILE *f = fopen(path, "wx"); /* x: lose the race rather than an edit */
    if (f) {
      char *text = config_dump_defaults();
      fputs(text, f);
      free(text);
      fclose(f);
      app_toast(a, "wrote a starting config");
    }
  }

  const char *editor =
      CFG.editor && *CFG.editor ? CFG.editor : getenv("EDITOR");
  /* vi is the one editor a POSIX system is required to have. Better a wrong
   * guess you can see and change than a pane that opens empty. */
  if (!editor || !*editor) editor = "vi";

  char cmd[1024];
  snprintf(cmd, sizeof cmd, "%s '%s'", editor, path);
  const char *argv[] = {"/bin/sh", "-c", cmd, NULL};

  /* Down rather than across: a config file is lines, and half the width of a
   * pane is a worse place to read them than half the height. */
  if (!split_fits(n, SPLIT_ROWS)) {
    app_toast(a, "no room to split down");
    return false;
  }
  node_t *fresh = split_node_with(a, n, SPLIT_ROWS, false, argv, cmd);
  if (!fresh) return false;
  pane_set_ephemeral(fresh->pane, true);
  return true;
}

bool app_rerun_pane(app_t *a, uint32_t id) {
  node_t *n = id ? pane_by_id(a, id) : (a->ntabs ? cur(a)->focus : NULL);
  if (!n) return false;
  bool ok =
      pane_suspended(n->pane) ? pane_start(n->pane) : pane_restart(n->pane);
  if (!ok) {
    app_toast(a, "cannot run it again");
    return false;
  }
  /* The new pty is born at the pane's current size: a dead pane is still laid
   * out and still resized, pane_resize() just had no pty to tell. */
  cur(a)->focus = n;
  return true;
}

uint32_t app_focused_pane_id(app_t *a) {
  return a->ntabs && cur(a)->focus ? cur(a)->focus->id : 0;
}

uint32_t app_current_tab_id(app_t *a) { return a->ntabs ? cur(a)->id : 0; }

/* ---- layouts ------------------------------------------------------------ *
 *
 *   layout {
 *       tab name="api" purpose="project:api.a1b2" cwd="/home/user/dev/api" {
 *           pane purpose="agent:main" command="pi" suspended=true
 *           pane split="rows" {
 *               pane command="npm run dev" suspended=true
 *               pane
 *           }
 *       }
 *   }
 *
 * A `pane` with children is a split; `split` on it picks the direction. Every
 * purpose a layout declares is locked, which is the point: identity comes from
 * the layout, not from whatever the program inside decides to print (D8).
 *
 * `base` is the directory the file came from, and a relative `cwd=` resolves
 * against it, so a layout can be checked in beside the project it describes
 * (path_resolve). A layout that arrived as text has no base and keeps the old
 * meaning: relative to wherever the session is.
 */

static node_t *build_pane(app_t *a, const kdl_node_t *node, const char *cwd,
                          const char *base) {
  /* Resolved once here, so both the pane spawned from this node and every
   * child that inherits the value get the same real directory. The buffer
   * outlives the recursion below it: children finish before we return. An
   * inherited value is already resolved, so it is passed through untouched --
   * resolving it twice would re-root a path against itself. */
  char cwdbuf[1024];
  const char *own = kdl_prop(node, "cwd", NULL);
  const char *node_cwd =
      own ? path_resolve(own, base, cwdbuf, sizeof cwdbuf) : cwd;

  /* a split: children, in order, in one direction */
  size_t kids = 0;
  for (size_t i = 0; i < node->nkids; i++)
    if (strcmp(node->kids[i]->name, "pane") == 0) kids++;

  if (kids) {
    node_t *sp = calloc(1, sizeof *sp);
    sp->kind = NODE_SPLIT;
    sp->id = ++a->next_id;
    /* A dumped layout carries the proportions it had; a hand-written one says
     * nothing and means "even", which is what WEIGHT_UNIT is. */
    sp->weight = (int)kdl_prop_int(node, "weight", WEIGHT_UNIT);
    if (sp->weight < WEIGHT_MIN) sp->weight = WEIGHT_UNIT;
    sp->dir = strcmp(kdl_prop(node, "split", "cols"), "rows") == 0 ? SPLIT_ROWS
                                                                   : SPLIT_COLS;
    for (size_t i = 0; i < node->nkids; i++) {
      if (strcmp(node->kids[i]->name, "pane") != 0) continue;
      node_t *kid = build_pane(a, node->kids[i], node_cwd, base);
      if (!kid) continue;
      sp->kids = realloc(sp->kids, (sp->nkids + 1) * sizeof *sp->kids);
      sp->kids[sp->nkids++] = kid;
      kid->parent = sp;
    }
    if (sp->nkids == 0) {
      free(sp);
      return NULL;
    }
    if (sp->nkids == 1) { /* a split of one is just the pane */
      node_t *only = sp->kids[0];
      only->parent = NULL;
      free(sp->kids);
      free(sp);
      return only;
    }
    return sp;
  }

  const char *command = kdl_prop(node, "command", NULL);
  bool suspended = a->force_suspend || kdl_prop_bool(node, "suspended", false);
  const char *argv[4];
  if (command) {
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = command;
    argv[3] = NULL;
  } else {
    const char *const *shell = default_argv(a);
    argv[0] = shell[0];
    argv[1] = NULL;
    for (size_t i = 1; shell[i] && i < 3; i++) argv[i] = shell[i];
  }

  node_t *leaf = leaf_new_ex(a, command ? argv : default_argv(a), node_cwd,
                             suspended, command ? command : "");
  if (!leaf) return NULL;
  leaf->weight = (int)kdl_prop_int(node, "weight", WEIGHT_UNIT);
  if (leaf->weight < WEIGHT_MIN) leaf->weight = WEIGHT_UNIT;
  /* `focus=true` restores which pane you were in. Recorded on the node and
   * resolved once the tab exists, because focus belongs to the tab. */
  if (kdl_prop_bool(node, "focus", false)) a->restore_focus = leaf;
  const char *purpose = kdl_prop(node, "purpose", NULL);
  if (purpose) {
    sanitise_purpose(purpose, leaf->purpose, sizeof leaf->purpose);
    leaf->purpose_locked = true; /* declared by a layout: in-band cannot win */
  }
  return leaf;
}

bool app_apply_layout(app_t *a, const kdl_node_t *root, bool replace,
                      const char *base, char *err, size_t errcap) {
  a->restore_tab = (size_t)-1;
  const kdl_node_t *lay = kdl_child(root, "layout");
  if (!lay) lay = root; /* allow a bare list of tabs */

  size_t before = a->ntabs;
  size_t made = 0;
  for (size_t i = 0; i < lay->nkids; i++) {
    const kdl_node_t *t = lay->kids[i];
    if (strcmp(t->name, "tab") != 0) continue;

    tab_t *tab = tab_add(a, kdl_prop(t, "name", ""));
    bool active = kdl_prop_bool(t, "active", false);
    const char *purpose = kdl_prop(t, "purpose", NULL);
    if (purpose) {
      sanitise_purpose(purpose, tab->purpose, sizeof tab->purpose);
      tab->purpose_locked = true;
    }

    /* The tab body is a split of its pane children, and the tab's own props
     * are its root's -- including `cwd`, which build_pane reads off the body
     * for itself. `base` as the inherited value is what makes a layout with no
     * `cwd` anywhere in it start in the directory it was checked into. */
    kdl_node_t body = {.name = (char *)"pane",
                       .kids = t->kids,
                       .nkids = t->nkids,
                       .props = t->props,
                       .nprops = t->nprops};
    node_t *tree = build_pane(a, &body, base, base);
    if (!tree) tree = leaf_new(a);
    if (!tree) {
      a->ntabs--;
      if (err) snprintf(err, errcap, "cannot create panes for tab %zu", i + 1);
      return false;
    }
    tab->root = tree;
    tab->focus = a->restore_focus ? a->restore_focus : first_leaf_of(tree);
    a->restore_focus = NULL;
    /* `active=true` restores which tab you were looking at. Resolved by index
     * because the tab was appended to whatever was already there. */
    if (active) a->restore_tab = (size_t)(tab - a->tabs);
    made++;
  }

  if (!made) {
    /* The other half of telling the two documents apart. A file with `theme` or
     * `keys` at the top of it is somebody's config, and "declares no tabs" is true
     * of it in the least useful way -- the same message a layout gets when its own
     * tabs are misspelled. `config_is_setting` answers from the loader's own list,
     * so this cannot drift from what a config actually holds. */
    for (size_t i = 0; i < root->nkids; i++) {
      const kdl_node_t *n = root->kids[i];
      if (n && n->name && config_is_setting(n->name)) {
        if (err)
          snprintf(err, errcap,
                   "this is a config, not a layout: `%s` is a setting",
                   n->name);
        return false;
      }
    }
    if (err) snprintf(err, errcap, "layout declares no tabs");
    return false;
  }

  if (replace) { /* drop the tabs that existed before this layout */
    for (size_t i = 0; i < before; i++) node_free(a->tabs[i].root);
    memmove(&a->tabs[0], &a->tabs[before], made * sizeof *a->tabs);
    a->ntabs = made;
  }
  /* Which tab to land on: the one a dump marked `active`, if it named one,
   * and otherwise the first of what was just built. Setting it unconditionally
   * here is what quietly undid the restore.
   *
   * The index was recorded while the new tabs sat *after* the old ones, and
   * `replace` has just moved them to the front -- so it shifts by exactly the
   * number that were dropped. Off by that, it lands on the wrong tab, which
   * looks like the restore working badly rather than not at all. */
  if (a->restore_tab != (size_t)-1 && replace && a->restore_tab >= before)
    a->restore_tab -= before;
  if (a->restore_tab != (size_t)-1 && a->restore_tab < a->ntabs)
    a->cur = a->restore_tab;
  else
    a->cur = replace ? 0 : before;
  a->restore_tab = (size_t)-1;
  layout(a);
  return true;
}

bool app_apply_layout_text_at(app_t *a, const char *text, bool replace,
                              const char *base, char *err, size_t errcap) {
  kdl_node_t *root = kdl_parse(text, err, errcap);
  if (!root) return false;
  bool ok = app_apply_layout(a, root, replace, base, err, errcap);
  kdl_free(root);
  return ok;
}

bool app_apply_layout_text(app_t *a, const char *text, bool replace, char *err,
                           size_t errcap) {
  /* No base: text has no directory it came from, so a relative `cwd=` in it
   * keeps meaning what it meant before there was a base to be relative to. */
  return app_apply_layout_text_at(a, text, replace, NULL, err, errcap);
}

bool app_apply_layout_file(app_t *a, const char *path, bool replace, char *err,
                           size_t errcap) {
  /* Expanded here rather than left to a shell: `--layout "~/x.layout.kdl"` in
   * quotes, and every path arriving over the control socket, reach fopen with
   * the tilde still on them. */
  char pathbuf[1024], dirbuf[1024];
  const char *file = path_expand(path, pathbuf, sizeof pathbuf);
  kdl_node_t *root = kdl_parse_file(file, err, errcap);
  if (!root) return false;
  bool ok = app_apply_layout(
      a, root, replace, path_dir(file, dirbuf, sizeof dirbuf), err, errcap);
  kdl_free(root);
  return ok;
}

/* ---- checking one -------------------------------------------------------- *
 *
 * The other half of D2's promise: a document says which one it is, and a name
 * the loader does not read is reported rather than skipped. build_pane() above
 * asks for props by name and ignores the rest, which is right for loading --
 * a session should start -- and useless for the person who wrote `cmd=` where
 * `command=` was meant and got a shell.
 *
 * These lists sit next to the code that reads them on purpose, and
 * `tests/test_layout_check.py` greps this file for every `kdl_prop*(node, ...)`
 * the layout section asks for and fails if one is missing here -- the same
 * arrangement that keeps KNOWN_TOP honest in config.c.
 */

/* Read by build_pane, on a `pane` node or on a tab acting as its own root. */
static const char *const PANE_PROPS[] = {
    "split", "weight", "cwd", "command", "focus", "purpose", "suspended"};
/* Read by app_apply_layout off the tab itself. */
static const char *const TAB_PROPS[] = {"name", "active"};

static bool in_list(const char *const *list, size_t n, const char *name) {
  for (size_t i = 0; i < n; i++)
    if (strcmp(list[i], name) == 0) return true;
  return false;
}

#define NELEM(x) (sizeof(x) / sizeof(*(x)))

typedef struct {
  const char *file;
  layout_msg_t *msgs;
  size_t max, n, dropped;
} lcheck_t;

static void lc_say(lcheck_t *c, int line, const char *fmt, ...) {
  if (c->n >= c->max) {
    c->dropped++;
    return;
  }
  char text[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  snprintf(c->msgs[c->n++], sizeof *c->msgs, "%s:%d: %s", c->file, line, text);
}

/* `true`/`false` and nothing else: kdl_prop_bool falls back silently, so
 * `suspended=yes` is a pane that quietly starts. */
static void lc_bool(lcheck_t *c, const kdl_node_t *n, const char *key) {
  const char *v = kdl_prop(n, key, NULL);
  if (v && strcmp(v, "true") != 0 && strcmp(v, "false") != 0)
    lc_say(c, n->line, "%s takes true or false, not `%s`", key, v);
}

static void lc_props(lcheck_t *c, const kdl_node_t *n, bool is_tab) {
  size_t kids = 0;
  for (size_t i = 0; i < n->nkids; i++)
    if (strcmp(n->kids[i]->name, "pane") == 0) kids++;

  for (size_t i = 0; i < n->nprops; i++) {
    const char *k = n->props[i].key;
    bool known = in_list(PANE_PROPS, NELEM(PANE_PROPS), k) ||
                 (is_tab && in_list(TAB_PROPS, NELEM(TAB_PROPS), k));
    if (!known) {
      lc_say(c, n->line, "unknown %s property: %s", is_tab ? "tab" : "pane", k);
      continue;
    }
    /* A split reads `split=` and its children; a leaf reads what it runs. The
     * wrong half is dropped without a word, which is how `purpose=` on a split
     * comes to tag nothing at all. A tab reads its own name and purpose either
     * way, so those are never the ignored ones. */
    bool leaf_only = strcmp(k, "command") == 0 || strcmp(k, "suspended") == 0 ||
                     strcmp(k, "focus") == 0 ||
                     (!is_tab && strcmp(k, "purpose") == 0);
    if (kids && leaf_only)
      lc_say(c, n->line, "%s is ignored on a %s with panes in it", k,
             is_tab ? "tab" : "pane");
    if (!kids && strcmp(k, "split") == 0)
      lc_say(c, n->line, "split is ignored on a pane with nothing to split");
  }

  const char *dir = kdl_prop(n, "split", NULL);
  if (dir && strcmp(dir, "cols") != 0 && strcmp(dir, "rows") != 0)
    lc_say(c, n->line, "split is cols or rows, not `%s`", dir);

  const char *w = kdl_prop(n, "weight", NULL);
  if (w) {
    char *end = NULL;
    long v = strtol(w, &end, 10);
    if (!end || *end || v < WEIGHT_MIN)
      lc_say(c, n->line, "weight is a number of %d or more, not `%s`",
             WEIGHT_MIN, w);
  }

  lc_bool(c, n, "suspended");
  lc_bool(c, n, "focus");
  if (is_tab) lc_bool(c, n, "active");

  for (size_t i = 0; i < n->nkids; i++) {
    const kdl_node_t *k = n->kids[i];
    if (strcmp(k->name, "pane") != 0) {
      lc_say(c, k->line, "a %s holds panes, not `%s`", is_tab ? "tab" : "pane",
             k->name);
      continue;
    }
    lc_props(c, k, false);
  }
}

size_t layout_check(const kdl_node_t *root, const char *file,
                    layout_msg_t *msgs, size_t max, size_t *dropped) {
  lcheck_t c = {.file = file, .msgs = msgs, .max = max};
  const kdl_node_t *lay = kdl_child(root, "layout");
  size_t tabs = 0;

  for (size_t i = 0; i < root->nkids; i++) {
    const kdl_node_t *n = root->kids[i];
    if (!n || !n->name) continue;
    if (lay && n == lay) continue;
    if (strcmp(n->name, "tab") == 0)
      continue; /* a bare list of tabs is legal */
    if (config_is_setting(n->name)) {
      lc_say(&c, n->line, "this is a config, not a layout: `%s` is a setting",
             n->name);
      continue;
    }
    lc_say(&c, n->line, "unknown node: %s", n->name);
  }

  if (!lay) lay = root;
  for (size_t i = 0; i < lay->nkids; i++) {
    const kdl_node_t *n = lay->kids[i];
    if (!n || !n->name) continue;
    if (strcmp(n->name, "tab") != 0) {
      if (lay != root)
        lc_say(&c, n->line, "a layout holds tabs, not `%s`", n->name);
      continue;
    }
    tabs++;
    lc_props(&c, n, true);
  }

  /* Last, so it reads as the summary it is rather than the first surprise. */
  if (!tabs) lc_say(&c, lay->line, "this layout declares no tabs");
  if (dropped) *dropped = c.dropped;
  return c.n;
}

size_t layout_check_file(const char *path, layout_msg_t *msgs, size_t max,
                         size_t *dropped) {
  char pathbuf[1024];
  const char *file = path_expand(path, pathbuf, sizeof pathbuf);
  const char *base = strrchr(file, '/');
  base = base ? base + 1 : file;

  char err[256] = {0};
  kdl_node_t *root = kdl_parse_file(file, err, sizeof err);
  if (!root) {
    /* kdl reports `line N: what`, with no filename because it never saw one,
     * and `cannot open X` for the other kind of failure -- which carries its
     * own path and no line. Same `file:line: text` shape as everything else
     * either way, so an editor's compile step reads all of them. */
    if (max) {
      int line = 0;
      if (sscanf(err, "line %d:", &line) == 1)
        snprintf(msgs[0], sizeof *msgs, "%s:%d:%s", base, line,
                 strchr(err, ':') + 1);
      else
        snprintf(msgs[0], sizeof *msgs, "%s", err);
    }
    if (dropped) *dropped = 0;
    return max ? 1 : 0;
  }
  size_t n = layout_check(root, base, msgs, max, dropped);
  kdl_free(root);
  return n;
}

/* ---- workspaces ---------------------------------------------------------- *
 *
 * A project is a directory (project.c). A workspace is the tab it occupies here.
 * Membership is the tab's *purpose*, in the `project:` namespace -- the shape
 * this repo has published as a project tab's identity since D3 -- so there is no
 * second answer to "what is this tab" and a dumped layout restores membership
 * for free, because a dump already writes tab purposes and apply-layout already
 * locks them (D8).
 *
 * Opening is idempotent, which is `sl0ppi up`'s property carried over: the same
 * request twice is one workspace, focused. Everything else follows from that --
 * a script can drive it in a loop without asking first, and so can a keystroke.
 */

/* Which roots to scan and how deep, from the config in force. Read fresh each
 * time rather than cached, because saving the config is allowed to change where
 * projects live and a cache would answer with yesterday's ~/dev. */
static const char *const *workspace_roots(int *depth) {
  static const char *roots[PROJECT_ROOTS_MAX + 1];
  size_t n = 0;
  for (; n < CFG.nproject_roots && n < PROJECT_ROOTS_MAX; n++)
    roots[n] = CFG.project_roots[n];
  roots[n] = NULL;
  if (depth) *depth = CFG.project_depth;
  return roots;
}

bool app_project_roots_set(void) { return CFG.nproject_roots > 0; }

size_t app_projects(project_t *out, size_t max) {
  int depth = 2;
  const char *const *roots = workspace_roots(&depth);
  return project_scan(roots, depth, out, max);
}

/* The first tab holding this workspace, or NULL: a workspace whose layout
 * declared several tabs has several, and the first is the one to land in. */
static tab_t *workspace_tab(app_t *a, const char *slug) {
  for (size_t i = 0; i < a->ntabs; i++)
    if (strcmp(a->tabs[i].purpose, slug) == 0) return &a->tabs[i];
  return NULL;
}

uint32_t app_workspace_tab(app_t *a, const char *slug) {
  tab_t *t = workspace_tab(a, slug);
  return t ? t->id : 0;
}

/* Make every tab this apply just built a member of the workspace.
 *
 * A tab the layout gave a purpose of its own keeps it and is not a member --
 * honoured rather than overwritten, because overwriting a declared purpose is
 * the one thing D8 forbids. The count comes back so the caller can say so
 * instead of it being a silent surprise. */
static void adopt_tabs(app_t *a, size_t from, const project_t *p,
                       app_workspace_open_t *out) {
  for (size_t i = from; i < a->ntabs; i++) {
    tab_t *t = &a->tabs[i];
    if (t->purpose[0] && strncmp(t->purpose, "project:", 8) != 0) {
      out->honoured++;
      continue;
    }
    sanitise_purpose(p->slug, t->purpose, sizeof t->purpose);
    t->purpose_locked = true;
    /* A tab with no name of its own takes the project's: a strip reading
     * `1 2 3` is not navigation, which is the whole complaint workspaces
     * answer. */
    if (!t->name[0]) snprintf(t->name, sizeof t->name, "%s", p->name);
    if (!out->tab) out->tab = t->id;
    out->tabs++;
  }
}

bool app_workspace_find(const char *name, project_t *out) {
  int depth = 2;
  const char *const *roots = workspace_roots(&depth);
  return project_find(roots, depth, name, out);
}

bool app_workspace_open(app_t *a, const char *name, bool suspended,
                        app_workspace_open_t *out, char *err, size_t errcap) {
  memset(out, 0, sizeof *out);
  project_t p;
  if (!app_workspace_find(name, &p)) {
    snprintf(err, errcap,
             CFG.nproject_roots ? "no project called %s"
                                : "no project roots: set project_roots in your "
                                  "config (asked for %s)",
             name);
    return false;
  }
  snprintf(out->purpose, sizeof out->purpose, "%s", p.slug);
  snprintf(out->path, sizeof out->path, "%s", p.path);

  /* Already open: focus it. The same request twice is one workspace, and the
   * answer says which branch it took so a caller never has to ask first. */
  tab_t *have = workspace_tab(a, p.slug);
  if (have) {
    out->tab = have->id;
    app_select_tab_id(a, have->id);
    return true;
  }

  /* `suspended` has to be decided before anything spawns: a pane is created
   * suspended or it is created running, and there is no un-running a process.
   * Carried on the app for the duration of one apply, the way restore_focus and
   * restore_tab already are, rather than widening two signatures for it. */
  a->force_suspend = suspended;
  size_t before = a->ntabs;
  bool ok;
  if (p.layout[0]) {
    ok = app_apply_layout_file(a, p.layout, false, err, errcap);
  } else if (CFG.project_layout) {
    /* The base is the *project*, not the directory this file came from: the
     * point of a shared project layout is that `cwd="."` means whichever
     * project is being opened. Relative to itself it would open every project
     * in ~/.config. */
    char buf[1024];
    kdl_node_t *root = kdl_parse_file(
        path_expand(CFG.project_layout, buf, sizeof buf), err, errcap);
    ok = root && app_apply_layout(a, root, false, p.path, err, errcap);
    kdl_free(root);
  } else {
    /* No file anywhere: one pane, your shell, in the project. Which is the
     * least a `.git` with nothing else in it can honestly be opened as. */
    char kdl[256];
    snprintf(kdl, sizeof kdl, "layout { tab { pane } }");
    ok = app_apply_layout_text_at(a, kdl, false, p.path, err, errcap);
  }
  a->force_suspend = false;
  if (!ok) return false;

  adopt_tabs(a, before, &p, out);
  out->created = true;
  if (out->tab) app_select_tab_id(a, out->tab);
  layout(a);
  return true;
}

size_t app_workspace_close(app_t *a, const char *slug) {
  size_t closed = 0;
  /* Re-found each time rather than collected first: closing a tab moves every
   * index after it, and a list of pointers taken beforehand would be stale by
   * the second one. */
  for (;;) {
    tab_t *t = workspace_tab(a, slug);
    if (!t || !app_close_tab(a, t->id)) break;
    closed++;
  }
  return closed;
}

/* Which project a tab belongs to, or is standing in.
 *
 * Three answers in order: the path asked for, the project whose slug this tab
 * already carries, and the directory the focused pane is in. All three are
 * resolved by *scanning*, which is also the containment check -- a directory no
 * root holds is one this session will not write a layout into, and that falls
 * out of asking the same question the picker asks rather than being a rule of
 * its own. */
static bool tab_project(app_t *a, const tab_t *t, const char *path,
                        project_t *out) {
  int depth = 2;
  const char *const *roots = workspace_roots(&depth);
  if (path && *path) return project_find(roots, depth, path, out);

  if (strncmp(t->purpose, "project:", 8) == 0) {
    project_t all[PROJECTS_MAX];
    size_t n = project_scan(roots, depth, all, PROJECTS_MAX);
    for (size_t i = 0; i < n; i++)
      if (strcmp(all[i].slug, t->purpose) == 0) {
        *out = all[i];
        return true;
      }
  }

  if (!t->focus) return false;
  char cwdbuf[4096];
  return project_find(roots, depth,
                      live_cwd(t->focus->pane, cwdbuf, sizeof cwdbuf), out);
}

bool app_workspace_save(app_t *a, uint32_t tab, const char *path, int suspend,
                        bool force, app_workspace_save_t *out, char *err,
                        size_t errcap) {
  memset(out, 0, sizeof *out);
  tab_t *t = tab ? tab_by_id(a, tab) : cur(a);
  if (!t) {
    snprintf(err, errcap, "no such tab");
    return false;
  }

  project_t p;
  if (!tab_project(a, t, path, &p)) {
    snprintf(err, errcap, "this tab is not in a project root");
    return false;
  }
  /* A tab that is already *a* workspace may only be saved into its own project.
   * Saving it elsewhere would leave one tab claiming two projects -- named for
   * one, carrying the other's purpose -- which is the second source of truth
   * this whole design exists to avoid. Almost always it is a `path` typed while
   * the wrong tab was focused, so it is said rather than obeyed. */
  if (strncmp(t->purpose, "project:", 8) == 0 &&
      strcmp(t->purpose, p.slug) != 0) {
    snprintf(
        err, errcap,
        "this tab is another project's workspace (%s): save it from its own"
        " tab, or pass that tab's id",
        t->purpose);
    return false;
  }
  snprintf(out->path, sizeof out->path, "%s/%s", p.path, PROJECT_LAYOUT_FILE);
  out->replaced = p.layout[0] != 0;
  if (out->replaced && !force) {
    snprintf(err, errcap, "%s already has a layout: pass force to replace it",
             p.name);
    return false;
  }

  dump_layout_t o = {
      .tab = t->id, .base = p.path, .suspend = suspend, .for_project = true};
  char *kdl = app_dump_layout(a, &o);
  if (!kdl || !o.tabs) {
    free(kdl);
    snprintf(err, errcap, "nothing to write");
    return false;
  }
  FILE *f = fopen(out->path, "w");
  if (!f) {
    free(kdl);
    snprintf(err, errcap, "cannot write %s", out->path);
    return false;
  }
  /* One line of provenance, and the thing to run when it stops working. No
   * timestamp, for the same reason the dump has none: a file that differs every
   * time is a bad diff, and this one is meant to be committed. */
  fputs("// What this project needs open, written by `save-workspace`.\n"
        "// Checked by `sl0ppty --check`; opened by `open-workspace`.\n",
        f);
  fputs(kdl, f);
  bool wrote = fclose(f) == 0;
  free(kdl);
  if (!wrote) {
    snprintf(err, errcap, "cannot write %s", out->path);
    return false;
  }

  out->panes = o.panes;
  out->suspended = o.suspended;
  /* Saving is also adopting: the tab that wrote the file is that project's
   * workspace from now on, so `C-a W` in a tab you happened to build in a
   * checkout is the whole of onboarding one. */
  if (!t->purpose[0] || strncmp(t->purpose, "project:", 8) == 0) {
    sanitise_purpose(p.slug, t->purpose, sizeof t->purpose);
    t->purpose_locked = true;
    if (!t->name[0]) snprintf(t->name, sizeof t->name, "%s", p.name);
  }
  snprintf(out->purpose, sizeof out->purpose, "%s", t->purpose);
  return true;
}

/* ---- resizing and reordering -------------------------------------------- */

static size_t index_in_parent(node_t *n) {
  for (size_t i = 0; i < n->parent->nkids; i++)
    if (n->parent->kids[i] == n) return i;
  return 0;
}

/* Move weight between two adjacent siblings, refusing to squeeze either out. */
/* `from` always shrinks. It stops at the floor rather than being pushed under
 * one: a pane below the floor collapses the whole tab into a list, and having
 * that happen because you nudged a divider one cell too far would read as the
 * session falling over rather than as a limit being reached. Every other
 * resizable thing simply stops, so this does too.
 *
 * The clamp lives here because both ways of resizing end up here — the mouse
 * through drag_edge and the keyboard through resize_focus — and a limit that
 * only one of them respected would be worse than none. */
static void transfer_weight(node_t *from, node_t *to, int amount) {
  long min_weight = WEIGHT_MIN;

  node_t *sp = from->parent;
  if (sp && sp->nkids >= 2) {
    uint16_t floor_ = sp->dir == SPLIT_COLS ? MIN_PANE_COLS : MIN_PANE_ROWS;
    uint16_t gap =
        sp->dir == SPLIT_COLS ? (uint16_t)(CFG.gap * CFG.gap_aspect) : CFG.gap;
    uint16_t span = sp->dir == SPLIT_COLS ? sp->rect.w : sp->rect.h;
    uint16_t gaps = (uint16_t)(gap * (sp->nkids - 1));
    uint16_t avail = span > gaps ? (uint16_t)(span - gaps) : span;
    long total = 0;
    for (size_t k = 0; k < sp->nkids; k++) total += sp->kids[k]->weight;
    /* Rounded up: the weight that buys exactly `floor_` cells is a fraction,
     * and rounding down buys one cell fewer — which is the cell that puts the
     * pane under the floor. */
    if (avail && total > 0) {
      long need = ((long)floor_ * total + avail - 1) / avail;
      if (need > min_weight) min_weight = need;
    }
  }

  if (from->weight - amount < min_weight)
    amount = (int)(from->weight - min_weight);
  if (amount <= 0) return; /* already at the floor: the nudge does nothing */
  from->weight -= amount;
  to->weight += amount;
}

/* Move the boundary next to the focused pane in the given direction. The pane
 * left of a boundary grows when it moves right — which is what a person means
 * by "wider", whichever side of it they are on. */
static void resize_focus(app_t *a, int dx, int dy) {
  node_t *n = cur(a)->focus;
  if (!n) return;
  split_dir_t want = dx ? SPLIT_COLS : SPLIT_ROWS;
  while (n->parent && !(n->parent->dir == want && n->parent->nkids >= 2))
    n = n->parent;
  if (!n->parent) return; /* nothing to resize against, in that direction */

  node_t *p = n->parent;
  size_t i = index_in_parent(n);
  int dir = dx ? dx : dy;

  if (i + 1 < p->nkids) {
    if (dir > 0)
      transfer_weight(p->kids[i + 1], n, WEIGHT_STEP);
    else
      transfer_weight(n, p->kids[i + 1], WEIGHT_STEP);
  } else if (i > 0) {
    if (dir > 0)
      transfer_weight(n, p->kids[i - 1], WEIGHT_STEP);
    else
      transfer_weight(p->kids[i - 1], n, WEIGHT_STEP);
  }
  layout(a);
}

/* ---- equalising ---------------------------------------------------------
 *
 * Every visible pane an even share of the rows and columns it competes for.
 *
 * A split's children are weighted by *how many visible panes are behind each
 * of them*, not given a weight each: one pane beside a column of three is a
 * quarter of the width, not half of it. Weighting per child instead would make
 * "even" mean something different at every depth — the pane on its own would
 * get as much room as the three sharing the other side, which is the layout a
 * person asks for this action to get *away* from.
 *
 * Exactly equal cell counts are not something a tree can always express: a
 * nested split pays for its own gaps out of its share, and a share that is not
 * a whole number of cells lands where the layout's remainder loop puts it. So
 * this is the even division the tree *can* express, which is why the action is
 * "give every pane an even share" and not "make every pane the same size".
 *
 * Minimised panes are not counted. They are out of the layout entirely and
 * cost a row in the strip rather than a share of the tree, so counting one
 * would hand its share to a pane nobody can see. Their own weight is still
 * evened, because that is the share they come back to.
 *
 * Returns the number of visible panes under `n`. */
static size_t equalize_node(node_t *n) {
  if (n->kind == NODE_LEAF) {
    n->weight = WEIGHT_UNIT;
    return n->minimized ? 0 : 1;
  }
  size_t vis = 0;
  for (size_t i = 0; i < n->nkids; i++) vis += equalize_node(n->kids[i]);
  /* A subtree with nothing visible in it is not laid out at all, so its weight
   * is never divided by — but it must not be zero either, or restoring the
   * pane inside it would give it no share to come back to. */
  n->weight = (int)(WEIGHT_UNIT * (vis ? vis : 1));
  return vis;
}

/* The current tab, like zooming and minimising: this is about the arrangement
 * you are looking at. False when there are no boundaries to move, so the
 * caller can say so rather than leaving a keystroke looking broken. */
bool app_equalize_splits(app_t *a) {
  node_t *root = cur(a)->root;
  if (!root || root->kind == NODE_LEAF) return false;
  equalize_node(root);
  layout(a);
  return true;
}

/* ---- rotating -----------------------------------------------------------
 *
 * A quarter turn clockwise, of the whole tab.
 *
 * Every split changes axis, and one of the two directions also reverses its
 * children — because that is what a quarter turn does to a stack. Turn a column
 * of panes clockwise and the one that was on top is now the one on the *right*,
 * so a row split becomes a column split read backwards; turn a row of them and
 * the leftmost becomes the top, so the order stands. Getting that asymmetry
 * wrong gives you a mirror image, which looks almost right and is not a
 * rotation: four turns would not come back.
 *
 * Four turns *do* come back, exactly, which is what makes this safe to press:
 * it is a permutation of the tree and nothing else. Weights travel with the
 * children they belong to, so a pane that had two thirds of the width has two
 * thirds of the height afterwards.
 *
 * Depth first, so a subtree is turned before the split above it reorders it —
 * the two operations are independent, but doing the children first keeps the
 * reversal reading as one thing rather than as something that has to be
 * reasoned about twice. */
static void rotate_node(node_t *n) {
  if (!n || n->kind == NODE_LEAF) return;
  for (size_t i = 0; i < n->nkids; i++) rotate_node(n->kids[i]);

  if (n->dir == SPLIT_ROWS) {
    for (size_t i = 0, j = n->nkids ? n->nkids - 1 : 0; i < j; i++, j--) {
      node_t *t = n->kids[i];
      n->kids[i] = n->kids[j];
      n->kids[j] = t;
    }
    n->dir = SPLIT_COLS;
  } else {
    n->dir = SPLIT_ROWS;
  }
}

/* The current tab, like zooming and equalising. False when there is nothing to
 * turn — one pane looks the same from every angle — and false when the turn
 * would not fit, which is the interesting case.
 *
 * A pane's share of one axis is not a share the other axis can always afford:
 * two rows split 19:6 are fine as rows and put the smaller one under the column
 * floor as columns, and a node that cannot give its children their floor takes
 * the *whole tab* down to a list (D6). So the turn is tried, the layout is asked
 * what it made of it, and a turn that flattened the tab is put back — the same
 * rule `split_fits` applies to splitting, and for the same reason: an action a
 * person just asked for should not be the thing that breaks the arrangement.
 *
 * Put back by turning three more times rather than by a second, mirrored
 * rotation. Four turns are the identity, so this *is* the inverse, and the
 * asymmetry between the two directions — the part that is easy to get wrong —
 * stays written down exactly once.
 *
 * A tab that was *already* a list is turned anyway: there is nothing to break,
 * and refusing would mean a window too small to lay out could never be turned
 * back into one that is. */
bool app_rotate_layout(app_t *a) {
  node_t *root = cur(a)->root;
  if (!root || root->kind == NODE_LEAF) return false;

  bool was_flat = a->flattened;
  rotate_node(root);
  layout(a);
  if (a->flattened && !was_flat) {
    rotate_node(root);
    rotate_node(root);
    rotate_node(root);
    layout(a);
    return false;
  }
  return true;
}

/* Turning because a person just asked for it, which is the only case that needs
 * telling why nothing happened. The two refusals are different facts and get
 * different words: one pane has no arrangement to turn, and a turn that would
 * flatten the tab was put back. Same split of responsibility as
 * `split_focus_ui`: the tree edit is quiet and the UI explains itself. */
static void rotate_layout_ui(app_t *a) {
  bool one_pane = cur(a)->root && cur(a)->root->kind == NODE_LEAF;
  if (!app_rotate_layout(a))
    app_toast(a, one_pane ? "nothing to turn" : "no room to turn it");
}

/* Reordering is a swap of two leaves, in place: their positions, weights and
 * parents trade, and everything else about them is untouched. */
static bool swap_panes(app_t *a, uint32_t id_a, uint32_t id_b) {
  if (id_a == id_b) return false;
  node_t *x = pane_by_id(a, id_a), *y = pane_by_id(a, id_b);
  if (!x || !y || !x->parent || !y->parent) return false;

  node_t *px = x->parent, *py = y->parent;
  size_t ix = index_in_parent(x), iy = index_in_parent(y);
  int wx = x->weight, wy = y->weight;

  px->kids[ix] = y;
  py->kids[iy] = x;
  y->parent = px;
  x->parent = py;
  y->weight = wx;
  x->weight = wy;
  layout(a);
  return true;
}

/* ---- focus -------------------------------------------------------------- */

struct dirsearch {
  node_t *from;
  int dx, dy;
  node_t *best;
  long best_score;
};

static void dir_cb(node_t *n, void *ud) {
  struct dirsearch *d = ud;
  if (n == d->from) return;
  /* centres, in a coordinate space where a row counts double so that
   * "nearest" means the same thing horizontally and vertically */
  long fx = d->from->rect.x * 2 + d->from->rect.w,
       fy = d->from->rect.y * 2 + d->from->rect.h;
  long nx = n->rect.x * 2 + n->rect.w, ny = n->rect.y * 2 + n->rect.h;
  long along = d->dx ? (nx - fx) * d->dx : (ny - fy) * d->dy;
  if (along <= 0) return; /* not in that direction */
  long across = d->dx ? labs(ny - fy) : labs(nx - fx);
  long score = along + across * 4; /* prefer straight ahead over diagonal */
  if (!d->best || score < d->best_score) {
    d->best = n;
    d->best_score = score;
  }
}

static void focus_dir(app_t *a, int dx, int dy) {
  if (!cur(a)->focus) return;
  struct dirsearch d = {cur(a)->focus, dx, dy, NULL, 0};
  walk(cur(a)->root, dir_cb, &d);
  if (d.best) cur(a)->focus = d.best;
}

struct nextsearch {
  node_t *from;
  node_t *first, *prev_match, *next;
  bool seen;
};
static void next_cb(node_t *n, void *ud) {
  struct nextsearch *s = ud;
  if (!s->first) s->first = n;
  if (s->seen && !s->next) s->next = n;
  if (n == s->from) s->seen = true;
}

static void focus_next(app_t *a) {
  struct nextsearch s = {cur(a)->focus, NULL, NULL, NULL, false};
  walk(cur(a)->root, next_cb, &s);
  cur(a)->focus = s.next ? s.next : s.first;
}

/* ---- drawing ------------------------------------------------------------ */

/* A pane's own status line and buttons, drawn in its bottom frame row.
 *
 * Two things end up here, because they are the same thing: what a *live* pane
 * asked for over OSC 5577, and what a *dead* one is offered instead. A dead
 * pane's own buttons are inert — clicking one would write a click report into
 * a pty that is closed — so the row is given over to the two verbs that do
 * still mean something: run it again, or let it go. Same row, same shape,
 * same budgeting, so a dead pane is not a second kind of frame.
 *
 * Buttons are budgeted from the right *before* the status text gets any
 * columns, and each registers its hit as it is painted — the same rule the
 * frame's own buttons follow, for the same reason. Rightmost is last in the
 * list, so what drops off a narrow frame is what is listed first: `close`
 * outlives `re-run`, because being unable to dismiss a dead pane is a trap
 * and being unable to re-run one is an inconvenience. */
struct row_btn {
  char label[40];
  char action[48];
};

static void draw_pane_status(app_t *a, screen_t *s, node_t *leaf, color_t fg,
                             bool focused) {
  struct row_btn row[8];
  size_t nbtn = 0;
  char status[256] = {0};
  bool dead = !pane_alive(leaf->pane) && !pane_suspended(leaf->pane);

  if (dead) {
    snprintf(row[nbtn].label, sizeof row[0].label, "re-run");
    snprintf(row[nbtn].action, sizeof row[0].action, "rerun:%u", leaf->id);
    nbtn++;
    snprintf(row[nbtn].label, sizeof row[0].label, "close");
    snprintf(row[nbtn].action, sizeof row[0].action, "close:%u", leaf->id);
    nbtn++;
    exit_words(leaf->pane, status, sizeof status);
  } else {
    const pane_button_t *btns = NULL;
    size_t n = pane_buttons(leaf->pane, &btns);
    for (size_t i = 0; i < n && nbtn < sizeof row / sizeof *row; i++) {
      snprintf(row[nbtn].label, sizeof row[0].label, "%s", btns[i].label);
      snprintf(row[nbtn].action, sizeof row[0].action, "btn:%u:%s", leaf->id,
               btns[i].id);
      nbtn++;
    }
    snprintf(status, sizeof status, "%s", pane_status(leaf->pane));
  }
  if (!nbtn && !*status) return;

  rect_t r = leaf->rect;
  if (r.w < 6 || r.h < 3) return;
  uint16_t y = (uint16_t)(r.y + r.h - 1);
  uint16_t left = (uint16_t)(r.x + 1), right = (uint16_t)(r.x + r.w - 1);

  /* right to left, so a button that does not fit is simply not drawn */
  uint16_t x = right;
  for (size_t i = nbtn; i-- > 0;) {
    uint16_t w = (uint16_t)(cells(row[i].label) + 2); /* [label] */
    if (x < left + w + 1) break;
    x = (uint16_t)(x - w - 1);
    char label[80];
    snprintf(label, sizeof label, "[%s]", row[i].label);
    bool hot = ptr_on(a, x, y, w, 1);
    uint16_t drawn =
        screen_text(s, x, y, label, focused || hot ? BTN_FG : fg,
                    focused || hot ? BTN_BG : BTN_BG_IDLE, hot ? ATTR_BOLD : 0);
    hit_add(&s->hits, x, y, drawn, 1, row[i].action);
  }

  if (*status && x > left + 1) {
    char buf[256];
    int len = snprintf(buf, sizeof buf, " %s ", status);
    uint16_t room = (uint16_t)(x - left);
    if (len > (int)room) {
      len = room;
      buf[len] = 0;
    }
    screen_text(s, left, y, buf,
                dead ? DEAD_C : (focused ? TITLE_FOCUS : TITLE_IDLE), NO_COLOR,
                dead ? ATTR_BOLD : 0);
  }
}

/* The split handle: the middle of a border, and the only part of it that
 * splits.
 *
 * The whole side used to be the button, which reads well until you count the
 * accidents. A border is the longest target on a screen, it sits between two
 * things you did mean to click, and the cost of brushing it was a changed
 * layout. The gesture is worth keeping -- the side you click is the side the
 * pane arrives on, which no single glyph can say -- so what changes is the size
 * of the target: a *place* on the edge rather than the whole edge. The guide
 * (`draw_split_guide`) thickens it on hover and reads its rect back from the
 * hit list, so there is one opinion about where it is (D6: one geometry).
 *
 * Three cells is the floor the pane buttons already settled on, for the reason
 * written there: a one-cell target is a thing you miss with a mouse. Seven is
 * the ceiling, because a third of a tall pane's border is most of that border
 * again, and the accidents come back with it. */
static uint16_t split_handle_len(uint16_t span) {
  uint16_t len = (uint16_t)(span / 3);
  if (len < 3) len = 3;
  if (len > 7) len = 7;
  if (len > span) len = span;
  return len;
}

/* One side's handle. False when the side is too short to hold one, which is a
 * pane `split_fits` is about to refuse anyway. The top row is not here: its
 * placement depends on what the title and the buttons left, so draw_frame does
 * it once those are known. */
static bool split_handle(const node_t *leaf, char side, rect_t *out) {
  rect_t r = leaf->rect;
  if (r.w < 4 || r.h < 4) return false;
  bool vert = side == 'l' || side == 'r';
  /* The span leaves the corners out: a corner is where two gaps cross and is a
   * resize target already. */
  uint16_t span = vert ? (uint16_t)(r.h - 2) : (uint16_t)(r.w - 2);
  if (span < 3) return false;
  uint16_t len = split_handle_len(span);
  uint16_t off = (uint16_t)((span - len) / 2);
  if (vert)
    *out = (rect_t){side == 'l' ? r.x : (uint16_t)(r.x + r.w - 1),
                    (uint16_t)(r.y + 1 + off), 1, len};
  else
    *out =
        (rect_t){(uint16_t)(r.x + 1 + off), (uint16_t)(r.y + r.h - 1), len, 1};
  return true;
}

/* What a press on this side is called. The top row answers to `title:` because
 * it is the drag handle too, and one press has to be able to become either. */
static void split_handle_action(const node_t *leaf, char side, char *buf,
                                size_t cap) {
  if (side == 't')
    snprintf(buf, cap, "title:%u:t", leaf->id);
  else
    snprintf(buf, cap, "border:%u:%c", leaf->id, side);
}

/* The split guide, in two stages, because a hover answers two questions.
 *
 * Anywhere on the side: the edge goes heavy and the handle thickens. That says
 * *where to click*, which is the thing you cannot otherwise know now that the
 * whole edge is no longer the button.
 *
 * On the handle: the dashed line and the arrow as well, which say *what will
 * happen*. So sweeping a border shows you the button without also drawing a
 * boundary nobody asked about, and the hint (`hint_for`) follows the same
 * split -- it is the caption on the second stage, not the first.
 *
 * Drawn *after* the pane's content, because the dashed line crosses it: the
 * first version painted under the terminal and was invisible. Hover only, so
 * an idle frame stays quiet. */
static void draw_split_guide(app_t *a, screen_t *s, node_t *leaf) {
  if (!a->ptr_valid) return;
  if (a->drag.kind == DRAG_TITLE || a->drag.kind == DRAG_SELECT ||
      a->drag.kind == DRAG_EDGE)
    return; /* a drag in progress means the pointer is busy */

  /* Arm on dwell, not on contact: a pointer merely crossing a border on its
   * way somewhere else should not make the screen flash. Holding the button
   * on a border is intent, so that skips the wait. */
  if (a->drag.kind != DRAG_BORDER &&
      now_ms_() - a->ptr_still_since < CFG.hover_delay_ms)
    return;

  /* Ask the hit list where the pointer is, using the entries this very pass
   * registered for this pane. Drawing and hit-testing cannot disagree, and
   * neither can drawing and *the layout*. */
  const char *action = hit_test(&s->hits, a->ptr_x, a->ptr_y);
  if (!action) return;
  char side = 0;
  bool on_handle = false;
  char want[48];
  snprintf(want, sizeof want, "border:%u:", leaf->id);
  if (strncmp(action, want, strlen(want)) == 0) {
    side = action[strlen(want)];
    on_handle = true;
  } else {
    snprintf(want, sizeof want, "brim:%u:", leaf->id);
    if (strncmp(action, want, strlen(want)) == 0) {
      side = action[strlen(want)];
    } else {
      /* The top row, whose handle wears the `title:` name because a press
       * there can still become a drag. Matched with the terminator checked, or
       * pane 1 would answer for pane 12. */
      snprintf(want, sizeof want, "title:%u", leaf->id);
      size_t n = strlen(want);
      if (strncmp(action, want, n) == 0 && (!action[n] || action[n] == ':')) {
        side = 't';
        on_handle = action[n] == ':';
      }
    }
  }
  if (!side) return;

  /* Nothing is offered that cannot be delivered: below the floor this pane
   * would only collapse, so the border simply stops being a button. */
  if (!split_fits(leaf, side_dir(side))) return;

  rect_t r = leaf->rect;
  if (r.w < 4 || r.h < 4) return;
  uint16_t x1 = (uint16_t)(r.x + r.w - 1), y1 = (uint16_t)(r.y + r.h - 1);
  color_t hi = GUIDE;

  /* Stage one: the armed edge, and the handle on it. */
  if (side == 'l' || side == 'r') {
    uint16_t bx = side == 'l' ? r.x : x1;
    for (uint16_t y = (uint16_t)(r.y + 1); y < y1; y++)
      screen_text(s, bx, y, "\u2503", hi, NO_COLOR, ATTR_BOLD);
  } else {
    uint16_t by = side == 't' ? r.y : y1;
    for (uint16_t x = (uint16_t)(r.x + 1); x < x1; x++)
      screen_text(s, x, by, "\u2501", hi, NO_COLOR, ATTR_BOLD);
  }

  /* The handle, made heavier, and — where that means claiming a cell the pane
   * was using — the hit for it registered here, because *this* is the frame in
   * which it is that size.
   *
   * Only the upright sides grow. A cell is about twice as tall as it is wide
   * (`gap_aspect`, and the split picker leans on the same fact), so one extra
   * column is a nudge and one extra row is a slab: a two-row bar across a top or
   * bottom border reads as a wall rather than as a thicker line, and eats a row
   * of somebody's output to say so. The block glyph is already the heaviest
   * thing a single cell can draw, and against `━` it is unmistakable, so a
   * horizontal handle stays one row and says it with ink instead of size.
   *
   * The rect is read back from the list rather than recomputed: draw_frame
   * placed it, and for the top row it places it wherever the title and the
   * buttons left room, so recomputing it here would be a second opinion about
   * where it is. Each cell is then painted only if the list still says it
   * belongs to the handle, which is what keeps a block off the pane's name.
   * Drawing asks the question the click is going to ask. */
  char act[48];
  split_handle_action(leaf, side, act, sizeof act);
  const hit_t *core = hit_find(&s->hits, act);
  if (core) {
    rect_t h = {core->x, core->y, core->w, core->h};
    if (side == 'l' || side == 'r') {
      uint16_t in = side == 'l' ? (uint16_t)(h.x + 1) : (uint16_t)(h.x - 1);
      hit_add(&s->hits, in, h.y, 1, h.h, act);
      if (in < h.x) h.x = in;
      h.w = 2;
    }
    /* The button carries its own verb. The arrow used to ride the dashed line,
     * which put the two halves of the message in two places: the handle said
     * *press here* and something over in the middle of the pane said *this is
     * what happens*. On the handle it is one object that explains itself, and it
     * is legible from the first stage -- brushing an edge now tells you which way
     * that button splits without drawing a boundary you did not ask about.
     *
     * Knocked out of the bar with ATTR_INVERSE rather than drawn in a second
     * colour: the block is solid `guide`, so a `guide` glyph on top of it would
     * be invisible, and inverting spends no new theme knob to get contrast that
     * follows whatever colour you set.
     *
     * Pointers rather than the matching triangles for left and right: U+25C0 and
     * U+25B6 carry emoji presentation and terminals widely render them
     * double-width, and screen_text books every chrome glyph as one cell — so a
     * cell that draws as two would shift the rest of the row. U+25B2 is already
     * the scroll indicator, so the vertical pair is known good here. */
    const char *arrow = side == 'l'   ? "\u25c4"
                        : side == 'r' ? "\u25ba"
                        : side == 't' ? "\u25b2"
                                      : "\u25bc";
    /* Along the handle it goes in the middle, which is where the pointer is
     * aiming. Across it, on the edge's own cell rather than the one over the
     * content: the arrow marks a boundary, and the boundary lands on the frame.
     * That distinction only exists on the upright pair, since those are the two
     * that grew a second cell. */
    bool vert = side == 'l' || side == 'r';
    uint16_t ax = vert ? (side == 'l' ? h.x : (uint16_t)(h.x + h.w - 1))
                       : (uint16_t)(h.x + h.w / 2);
    uint16_t ay = vert ? (uint16_t)(h.y + h.h / 2) : h.y;
    for (uint16_t y = h.y; y < h.y + h.h; y++) {
      for (uint16_t x = h.x; x < h.x + h.w; x++) {
        const char *own = hit_test(&s->hits, x, y);
        if (!own || strcmp(own, act) != 0) continue;
        bool tip = x == ax && y == ay;
        screen_text(s, x, y, tip ? arrow : "\u2588", hi, NO_COLOR,
                    tip ? (uint16_t)(ATTR_BOLD | ATTR_INVERSE) : ATTR_BOLD);
      }
    }
  }

  if (!on_handle) return;

  /* Stage two: where the new boundary lands, and nothing about direction -- the
   * handle is holding that end of the message now. A plain dashed line is also
   * the honest shape for it: it is the *edge* of the new pane, and an edge has
   * no middle worth marking. */
  if (side == 'l' || side == 'r') {
    uint16_t mid = (uint16_t)(r.x + r.w / 2);
    for (uint16_t y = (uint16_t)(r.y + 1); y < y1; y++)
      screen_text(s, mid, y, "\u254e", hi, NO_COLOR, 0);
  } else {
    uint16_t mid = (uint16_t)(r.y + r.h / 2);
    for (uint16_t x = (uint16_t)(r.x + 1); x < x1; x++)
      screen_text(s, x, mid, "\u254c", hi, NO_COLOR, 0);
  }
}

static void draw_frame(app_t *a, screen_t *s, node_t *leaf) {
  rect_t r = leaf->rect;
  if (r.w < 3 || r.h < 3) return;
  bool focused = leaf == cur(a)->focus;
  bool drop_target = a->drag.kind == DRAG_TITLE && a->drag.target == leaf->id &&
                     a->drag.src != leaf->id;
  color_t fg = drop_target ? DROP_C : (focused ? FRAME_FOCUS : FRAME_IDLE);
  uint16_t attrs = drop_target ? ATTR_BOLD : 0;

  /* While a pane is being dragged, every other pane is somewhere it could be
   * dropped, and a dashed border says so without needing a legend. The pane in
   * your hand keeps a solid one, so the two states are told apart by the frame
   * as well as by the colour — which matters on a terminal whose palette makes
   * the greying subtle. The pane actually under the pointer still takes the
   * drop_target highlight on top of the dashes: these are all targets, that is
   * the one you are on. */
  bool drag_target =
      a->drag.kind == DRAG_TITLE && a->drag.moved && a->drag.src != leaf->id;
  const char *hbar = drag_target ? "\u2504" : "\u2500";
  const char *vbar = drag_target ? "\u2506" : "\u2502";

  const char *tl = CFG.rounded ? "╭" : "┌", *tr = CFG.rounded ? "╮" : "┐";
  const char *bl = CFG.rounded ? "╰" : "└", *br = CFG.rounded ? "╯" : "┘";

  uint16_t x1 = (uint16_t)(r.x + r.w - 1), y1 = (uint16_t)(r.y + r.h - 1);
  screen_text(s, r.x, r.y, tl, fg, NO_COLOR, attrs);
  screen_text(s, x1, r.y, tr, fg, NO_COLOR, attrs);
  screen_text(s, r.x, y1, bl, fg, NO_COLOR, attrs);
  screen_text(s, x1, y1, br, fg, NO_COLOR, attrs);
  for (uint16_t x = (uint16_t)(r.x + 1); x < x1; x++) {
    screen_text(s, x, r.y, hbar, fg, NO_COLOR, attrs);
    screen_text(s, x, y1, hbar, fg, NO_COLOR, attrs);
  }
  for (uint16_t y = (uint16_t)(r.y + 1); y < y1; y++) {
    screen_text(s, r.x, y, vbar, fg, NO_COLOR, attrs);
    screen_text(s, x1, y, vbar, fg, NO_COLOR, attrs);
  }

  /* A pane that rang, marked just inside its corner: the same place on every
   * pane, whatever its title is doing. */
  if (CFG.bell_indicator && pane_bell(leaf->pane) && r.w > 4)
    screen_text(s, (uint16_t)(r.x + 1), r.y, CFG.bell_mark, BELL_C, NO_COLOR,
                ATTR_BOLD);

  /* The frame's top row is the drag handle, all of it. Its *split* is a handle
   * on the same row, but it is placed at the very end of this function rather
   * than here: the title, the buttons and the scroll indicator all anchor
   * wherever the config puts them, a centred title lands exactly where a
   * centred handle would want to be, and this row's rule is that the title
   * wins. So the handle is put where they are not, once they are all placed. */
  {
    char action[48];
    snprintf(action, sizeof action, "title:%u", leaf->id);
    hit_add(&s->hits, r.x, r.y, r.w, 1, action);
  }

  /* No split button. The border *is* the button -- or the middle of it is:
   * clicking a side's handle splits toward that side, which encodes the
   * direction a single glyph never could, and gives every frame its columns
   * back. See split_handle(). */
  uint16_t avail = (uint16_t)(r.w - 2);
  bool has_btn = false;
  uint16_t btn_x = x1;
  /* Where the right-anchored group (buttons, then the scroll indicator) starts,
   * and what the title took. Both are needed at the end of this function to
   * find the top row's free cells; the corner counts as taken, hence x1. */
  uint16_t right_lo = x1;
  bool has_title = false;
  uint16_t title_lo = 0, title_hi = 0;

  /* The frame's own buttons, hard against the right of the top border. Three
   * cells each: a one-cell target is a thing you miss with a mouse, and the
   * spaces double as the gap between them. Budgeted before the scroll
   * indicator, which then places itself to their left — the same right-first
   * rule the tab strip and the status line use. */
  if (CFG.pane_buttons && avail > 10) {
    struct {
      const char *mark;
      const char *verb;
    } btns[] = {
        {CFG.close_mark, "close"},
        {app_pane_zoomed(a, leaf->id) ? CFG.zoom_on_mark : CFG.zoom_mark,
         "zoom"},
        {CFG.min_mark, "minimize"},
    };
    uint16_t bx = (uint16_t)(x1 - 1);
    for (size_t i = 0; i < sizeof btns / sizeof *btns; i++) {
      char cell[24];
      /* One space, not two. The gap between two marks is blank cells plus
       * whatever blank each glyph carries inside its own cell, and that second
       * part is not the same for a low underscore as for a square that fills
       * its box. Two cells made the difference plain; one makes it small, and
       * gives three columns of every frame back at the same time. */
      snprintf(cell, sizeof cell, "%s ", btns[i].mark);
      /* Measured in cells rather than assumed: a mark is yours to choose and
       * may be more than one character. Booking three cells for a
       * two-character mark would draw it over its neighbour and hand the
       * neighbour's hit a cell it does not own. */
      uint16_t mw = cells(btns[i].mark);
      if (!mw) mw = 1;
      uint16_t bw = (uint16_t)(mw + 1);
      if (bx < r.x + 4 + bw) break;
      uint16_t px = (uint16_t)(bx - bw + 1);
      bool hot = ptr_on(a, px, r.y, bw, 1);
      screen_text(s, px, r.y, cell, hot ? PANE_BTN_HOVER : PANE_BTN, NO_COLOR,
                  hot ? ATTR_BOLD : 0);
      char action[48];
      snprintf(action, sizeof action, "%s:%u", btns[i].verb, leaf->id);
      hit_add(&s->hits, px, r.y, bw, 1, action);
      bx = (uint16_t)(px - 1);
      has_btn = true;
      btn_x = px;
      avail = (uint16_t)(avail > bw ? avail - bw : 0);
    }
    /* One blank between the rule and the first button, so the group is not
     * welded to the frame the way a title without its space would be. */
    if (has_btn && btn_x > (uint16_t)(r.x + 1)) {
      screen_text(s, (uint16_t)(btn_x - 1), r.y, " ", PANE_BTN, NO_COLOR, 0);
      btn_x = (uint16_t)(btn_x - 1);
      avail = (uint16_t)(avail > 1 ? avail - 1 : 0);
    }
    if (has_btn) right_lo = btn_x;
  }

  /* Each side twice over: `brim:` is the whole of it, which arms the guide on
   * hover and does nothing at all on a click, and the handle on top of it is
   * what splits. Registered in that order so the handle wins its own cells.
   *
   * The rim is a target rather than nothing so that hovering anywhere on an
   * edge can still show you where the button is. Take it away and the handle
   * becomes a thing you have to already know about. */
  {
    char action[48];
    snprintf(action, sizeof action, "brim:%u:l", leaf->id);
    hit_add(&s->hits, r.x, (uint16_t)(r.y + 1), 1, (uint16_t)(r.h - 2), action);
    snprintf(action, sizeof action, "brim:%u:r", leaf->id);
    hit_add(&s->hits, x1, (uint16_t)(r.y + 1), 1, (uint16_t)(r.h - 2), action);
    snprintf(action, sizeof action, "brim:%u:b", leaf->id);
    hit_add(&s->hits, r.x, y1, r.w, 1, action);
    for (const char *side = "lrb"; *side; side++) {
      rect_t h;
      if (!split_handle(leaf, *side, &h)) continue;
      split_handle_action(leaf, *side, action, sizeof action);
      hit_add(&s->hits, h.x, h.y, h.w, h.h, action);
    }
  }

  draw_pane_status(a, s, leaf, fg, focused);

  /* Scroll position, when there is one: compact, right-aligned, and clickable
   * to get back to the bottom. Budgeted after the button and before the title,
   * because a title is the thing you can most afford to lose. */
  if (pane_scrolled(leaf->pane) && avail >= 8) {
    uint32_t above = 0, total = 0;
    pane_scroll_pos(leaf->pane, &above, &total);
    /* A space between the arrow and the count. U+25B2 is drawn wide enough in
     * plenty of fonts to touch whatever follows it, and "▲12" then reads as
     * one smudged token rather than an arrow and a number. */
    char num[16];
    int nd = snprintf(num, sizeof num, "%u", total > above ? total - above : 0);
    if (nd < 0) nd = 0;
    char ind[24];
    snprintf(ind, sizeof ind, " \u25b2 %s ", num);
    /* Counted in cells, not bytes: the arrow is three bytes and one column,
     * and measuring it with strlen reserved two columns that were never used. */
    uint16_t iw = (uint16_t)(nd + 4); /* space, arrow, space, digits, space */
    if (iw + 2 < avail) {
      uint16_t ix = (uint16_t)((has_btn ? btn_x : x1) - iw);
      uint16_t drawn =
          screen_text(s, ix, r.y, ind, SCROLL_FG, SCROLL_BG, ATTR_BOLD);
      char action[48];
      snprintf(action, sizeof action, "scrollbottom:%u", leaf->id);
      hit_add(&s->hits, ix, r.y, drawn, 1, action);
      right_lo = ix;
      avail = (uint16_t)(avail - iw);
    }
  }

  const char *title = pane_title(leaf->pane);
  bool naming = a->renaming == RENAME_PANE && a->rename_id == leaf->id;
  bool tagging = a->renaming == RENAME_PURPOSE && a->rename_id == leaf->id;
  bool editing = naming || tagging;
  if ((editing || (title && *title)) && avail >= 3) {
    char buf[320]; /* a full-length name, plus the caret and its spaces */
    /* The purpose editor says which label it is: typed into the same cell as a
     * rename, an unlabelled caret would leave you guessing which one you are
     * changing -- and the two are edited from keys one shift apart. */
    int len =
        tagging ? snprintf(buf, sizeof buf, " purpose %s\u2588 ", a->rename_buf)
        : naming ? snprintf(buf, sizeof buf, " %s\u2588 ", a->rename_buf)
                 : snprintf(buf, sizeof buf, " %s ", title);
    /* snprintf reports what it *would* have written; clamp to what it did, or
     * the scroll below would move bytes that are not there. */
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof buf) len = (int)strlen(buf);
    if (len > (int)avail) {
      if (editing) {
        /* Scroll the head off, not the tail: the cursor is the one thing that
         * must stay on screen while typing. Whole characters only. */
        size_t off = (size_t)(len - (int)avail);
        while (off < (size_t)len && ((unsigned char)buf[off] & 0xC0) == 0x80)
          off++;
        memmove(buf, buf + off, (size_t)len - off + 1);
        len = (int)((size_t)len - off);
      } else {
        len = (int)avail;
        buf[len] = 0;
      }
    }
    /* The inset is an edge offset, so it applies to whichever edge the title
     * is anchored to and means nothing in the middle. */
    uint16_t inset = CFG.title_inset < avail ? CFG.title_inset : 0;
    uint16_t tx = (uint16_t)(r.x + 1 + inset);
    if (CFG.title_align == ALIGN_CENTER)
      tx = (uint16_t)(r.x + 1 + (avail - len) / 2);
    else if (CFG.title_align == ALIGN_RIGHT)
      tx = (uint16_t)(r.x + 1 + avail - len - inset);
    /* An editor announces itself: the name sits in the button colours while it
     * is being typed, so a half-finished rename can never be mistaken for what
     * the pane is actually called. */
    uint16_t drawn = screen_text(
        s, tx, r.y, buf,
        editing ? RENAME_FG : (focused ? TITLE_FOCUS : TITLE_IDLE),
        editing ? RENAME_BG : NO_COLOR, editing || focused ? ATTR_BOLD : 0);

    /* The title names the pane; it is not an edge. It carves its own cells out
     * of the top row's handle so that resting there arms no split guide and
     * clicking there splits nothing, which leaves the gesture free for a
     * double-click rename — otherwise a rename would split twice on its way.
     * Dragging still moves the pane: that reads as grabbing it by its name.
     *
     * Registered last, because hit_test() scans backwards and the title is
     * painted last too. Hit-testing therefore agrees with what is on screen,
     * and the width comes from screen_text() so a title of wide characters
     * claims the cells it actually drew. */
    if (drawn) {
      char action[48];
      snprintf(action, sizeof action, "panetitle:%u", leaf->id);
      hit_add(&s->hits, tx, r.y, drawn, 1, action);
      has_title = true;
      title_lo = tx;
      title_hi = (uint16_t)(tx + drawn - 1);
    }
  }

  /* The top row's split handle, last of all, in whatever the row has left.
   *
   * The other three sides put it in the middle, which is where a person looks
   * for it. This row cannot promise that: the buttons own its right, and a
   * centred title -- the default -- owns exactly the cells the middle would
   * want. The title winning is the rule (see above: that is what keeps a
   * double-click rename from splitting twice on its way), so the handle takes
   * the widest run the row still owns and, between equals, the one nearest the
   * middle. A row with nothing left offers no upward split, and its other three
   * edges are unaffected. */
  {
    uint16_t lo = (uint16_t)(r.x + 1);
    if (CFG.bell_indicator && pane_bell(leaf->pane) && r.w > 4) lo++;
    struct run {
      uint16_t lo, hi;
    } runs[2];
    size_t nruns = 0;
    if (has_title) {
      if (title_lo > lo)
        runs[nruns++] = (struct run){lo, (uint16_t)(title_lo - 1)};
      if ((uint16_t)(title_hi + 1) < right_lo)
        runs[nruns++] =
            (struct run){(uint16_t)(title_hi + 1), (uint16_t)(right_lo - 1)};
    } else if (lo < right_lo) {
      runs[nruns++] = (struct run){lo, (uint16_t)(right_lo - 1)};
    }

    uint16_t mid = (uint16_t)(r.x + r.w / 2);
    uint16_t best_lo = 0, best_len = 0, best_dist = 0;
    for (size_t i = 0; i < nruns; i++) {
      if (runs[i].hi < runs[i].lo) continue;
      uint16_t len = (uint16_t)(runs[i].hi - runs[i].lo + 1);
      if (len < 3) continue;
      uint16_t c = (uint16_t)(runs[i].lo + len / 2);
      uint16_t dist = (uint16_t)(c > mid ? c - mid : mid - c);
      if (len > best_len || (len == best_len && dist < best_dist)) {
        best_lo = runs[i].lo;
        best_len = len;
        best_dist = dist;
      }
    }
    if (best_len >= 3) {
      uint16_t want = split_handle_len((uint16_t)(r.w - 2));
      if (want > best_len) want = best_len;
      char action[48];
      split_handle_action(leaf, 't', action, sizeof action);
      hit_add(&s->hits, (uint16_t)(best_lo + (best_len - want) / 2), r.y, want,
              1, action);
    }
  }
}

struct draw {
  app_t *a;
  screen_t *s;
};

/* Defined with the other shader passes below, because it is one. A collapsed
 * row needs it here: the row is chrome, and it is the only thing a flattened
 * tab draws of a pane. */
static void shade_chrome(app_t *a, screen_t *s, node_t *n, rect_t r,
                         const rect_t *hole);

/* A collapsed subtree: one row, the title of the pane it stands for, and a hit
 * that focuses it — which expands it on the next layout pass, because the
 * expanded child is simply the one holding focus. */
static void draw_collapsed(app_t *a, screen_t *s, node_t *n) {
  node_t *leaf = first_leaf_of(n);
  if (!leaf) return;
  rect_t r = n->rect;
  if (r.w < 4) return;

  const char *title = pane_title(leaf->pane);
  const char *status = pane_status(leaf->pane);
  /* A pane that is not live says so here, in place of whatever its program
   * last asked us to show — which is stale by definition once the program is
   * gone or was never started.
   *
   * This row is the only thing a flattened tab draws of a pane, and the
   * *content* shader pass deliberately never reaches it: those colour
   * contents, and a header is chrome (D13). So the states that get a colour
   * everywhere else have to be carried by the words here, in the same order
   * the status line ranks them. Without this, collapsing a tab hides exactly
   * the facts the colour exists to show. A `where="chrome"` chain does reach
   * this row — it is a frame — but that is a thing you may have asked for,
   * not something the words can rely on. */
  bool dead = !pane_alive(leaf->pane) && !pane_suspended(leaf->pane);
  char words[64];
  if (dead) {
    exit_words(leaf->pane, words, sizeof words);
    status = words;
  } else if (pane_suspended(leaf->pane)) {
    status = "not started";
  } else if (pane_scrolled(leaf->pane)) {
    uint32_t above = 0, total = 0;
    pane_scroll_pos(leaf->pane, &above, &total);
    snprintf(words, sizeof words, "\u25b2 %u",
             total > above ? total - above : 0);
    status = words;
  }
  char line[256];
  /* A space on each side, always. A title welded to the rule beside it reads
   * as one longer word, and the rule is not part of the name. */
  snprintf(line, sizeof line, " %s%s%s ", title && *title ? title : "pane",
           status && *status ? " · " : "", status && *status ? status : "");

  /* Once a tab is a list, its rows are what you are picking from, and a row
   * under the pointer should say so. Immediate rather than on dwell: this is
   * feedback about where the pointer *is*, not an action being armed, and a
   * list you are scanning should track the mouse continuously — the dwell that
   * stops a guide from flashing would only make this feel broken.
   *
   * Tested against the rect registered as the hit two lines below, so the row
   * that lights up and the row that would be clicked are the same row by
   * construction. A drag already owns the pointer and says nothing here. */
  bool hot = ptr_on(a, r.x, r.y, r.w, 1);

  /* Foreground only. A filled bar is louder than the thing it is telling you,
   * and this row has to sit in a list of its own kind without shouting. */
  color_t rule = hot ? HEADER_HOVER : HEADER;
  color_t label = hot ? HEADER_HOVER_TITLE : (dead ? DEAD_C : HEADER);

  /* A collapsed pane draws the top edge of a pane, corners and all. It is not
   * decoration: this row *is* a pane, closed — so a stack of them reads as a
   * stack of panes rather than as a list of labels that happen to be above
   * one. The open pane below wears the same corners. */
  const char *tl = CFG.rounded ? "╭" : "┌", *tr = CFG.rounded ? "╮" : "┐";
  uint16_t x1 = (uint16_t)(r.x + r.w - 1);
  for (uint16_t x = r.x; x <= x1; x++)
    screen_text(s, x, r.y, "─", rule, NO_COLOR, 0);
  screen_text(s, r.x, r.y, tl, rule, NO_COLOR, 0);
  screen_text(s, x1, r.y, tr, rule, NO_COLOR, 0);

  /* Just inside the corner, where it is in the same place on every pane
   * whatever its title is doing. */
  if (CFG.bell_indicator && pane_bell(leaf->pane) && r.w > 4)
    screen_text(s, (uint16_t)(r.x + 1), r.y, CFG.bell_mark, BELL_C, NO_COLOR,
                ATTR_BOLD);

  uint16_t tx = (uint16_t)(r.x + 1 + CFG.title_inset);
  if (tx < x1) {
    size_t room = (size_t)(x1 - tx); /* never over the closing corner */
    if (strlen(line) > room) line[room] = 0;
    screen_text(s, tx, r.y, line, label, NO_COLOR, hot ? ATTR_BOLD : 0);
  }

  char action[48];
  snprintf(action, sizeof action, "focus:%u", leaf->id);
  hit_add(&s->hits, r.x, r.y, r.w, 1, action);

  /* Last, so the pass sees the finished row: rule, bell, title and all. The
   * state comes from the leaf this row stands for, not from the node that
   * happens to own the rect — a collapsed subtree is drawn as its first pane
   * and should be coloured as that pane. */
  shade_chrome(a, s, leaf, r, NULL);
}

/* ---- shaders ------------------------------------------------------------ */

/* A chain the program in the pane asked for, in the config's own syntax so that
 * what you prototype is what you can paste (D13, revisited). Replaces that
 * chain: empty text clears it, which is how a program puts a pane back.
 *
 * Refused, with a reason, when `in_band_shaders` is off -- a program restyling
 * itself by accident is exactly what that decision was about, and the answer is
 * a line of config rather than a rule nobody can lift. */
/* A preset file, applied to this pane: both chains at once, routed by each entry's
 * own `where=`. The session reads the file, because the session has the parser --
 * a program that wants to prototype should not have to reimplement KDL, and a
 * second reader of the format would be a second answer about what a file says.
 *
 * Gated like setting a chain by hand, and for the same reason: this is the program
 * in the pane restyling the pane. Relative paths resolve against the *session's*
 * working directory, which is why `contrib/shader-repl` sends an absolute one --
 * the pane's own cwd is the pane's business and the session cannot see it. */
bool app_load_pane_shaders(app_t *a, uint32_t pane_id, const char *path,
                           size_t *nchrome, size_t *ncontent, char *err,
                           size_t errcap) {
  if (nchrome) *nchrome = 0;
  if (ncontent) *ncontent = 0;
  if (err && errcap) err[0] = 0;
  if (!CFG.in_band_shaders) {
    if (err && errcap) snprintf(err, errcap, "in_band_shaders is off");
    return false;
  }
  node_t *n = pane_by_id(a, pane_id ? pane_id : app_focused_pane_id(a));
  if (!n) {
    if (err && errcap) snprintf(err, errcap, "no such pane");
    return false;
  }
  if (!path || !path[0]) {
    if (err && errcap) snprintf(err, errcap, "no path");
    return false;
  }

  /* Both chains built beside the live ones and swapped in together, so a file
   * that turns out to be unreadable half way through leaves the pane as it was
   * rather than wearing the half that parsed. */
  inband_chain_t con = {0}, chr = {0};
  expr_prog_t *exprs[SHADE_MAX * 2];
  size_t nexprs = 0;
  size_t got =
      config_parse_chain_file(path, CFG.frame_focus, con.sh, &con.n, chr.sh,
                              &chr.n, exprs, &nexprs, err, errcap);
  if (!got) {
    for (size_t i = 0; i < nexprs; i++) expr_free(exprs[i]);
    if (err && errcap && !err[0]) snprintf(err, errcap, "nothing in it to run");
    return false;
  }

  /* The programs belong to whichever chain holds the shader that points at one.
   * Walking the two chains rather than trusting the order they came back in: a
   * chain that frees a program another chain is still using is the one bug in
   * this that would not show up until the next repaint. */
  for (size_t i = 0; i < nexprs; i++) {
    bool mine = false;
    for (size_t j = 0; j < chr.n && !mine; j++)
      if (chr.sh[j].amount_expr == exprs[i]) {
        chr.exprs[chr.nexprs++] = exprs[i];
        mine = true;
      }
    for (size_t j = 0; j < con.n && !mine; j++)
      if (con.sh[j].amount_expr == exprs[i]) {
        con.exprs[con.nexprs++] = exprs[i];
        mine = true;
      }
    if (!mine) expr_free(exprs[i]); /* its entry was dropped */
  }

  chain_clear(&n->chrome_chain);
  chain_clear(&n->content_chain);
  n->chrome_chain = chr;
  n->content_chain = con;
  pane_touch(n->pane);
  if (nchrome) *nchrome = chr.n;
  if (ncontent) *ncontent = con.n;
  return true;
}

/* Everything this pane painted on itself, gone: both chains at once.
 *
 * Not gated on `in_band_shaders`, unlike setting one. A chain can outlive the
 * consent that allowed it -- set it, then turn the setting off, and the paint is
 * still there -- so the way out must not be the thing the setting controls. It is
 * also the operator's answer to a pane that has made itself unreadable, and an
 * answer you have to edit a config to reach is not one.
 *
 * Says nothing about the config's own chains or the session's policy passes:
 * those are not this pane's doing and are derived every frame anyway. */
bool app_clear_pane_shaders(app_t *a, uint32_t pane_id) {
  node_t *n = pane_by_id(a, pane_id ? pane_id : app_focused_pane_id(a));
  if (!n) return false;
  bool had = n->content_chain.n || n->chrome_chain.n;
  chain_clear(&n->content_chain);
  chain_clear(&n->chrome_chain);
  if (had) pane_touch(n->pane);
  return had;
}

bool app_set_pane_shaders(app_t *a, uint32_t pane_id, bool default_chrome,
                          const char *text, size_t *nchrome, size_t *ncontent,
                          char *err, size_t errcap) {
  if (nchrome) *nchrome = 0;
  if (ncontent) *ncontent = 0;
  if (err && errcap) err[0] = 0;
  if (!CFG.in_band_shaders) {
    if (err && errcap) snprintf(err, errcap, "in_band_shaders is off");
    return false;
  }
  node_t *n = pane_by_id(a, pane_id);
  if (!n) {
    if (err && errcap) snprintf(err, errcap, "no such pane");
    return false;
  }

  /* Both chains, because the text is a *document*: `where=` inside it decides
   * where each pass goes, exactly as it does in the config file this syntax comes
   * from, and `default_chrome` is only what a pass that keeps quiet means. An
   * earlier version refused an entry that named the other rect, which made the
   * prompt's mode a rule rather than a default -- and made a two-rect block, the
   * thing `:paste` prints, impossible to paste back.
   *
   * Parsed beside the live chains and swapped in together, so text that turns out
   * to be unreadable leaves the pane as it was rather than half-restyled. */
  inband_chain_t con = {0}, chr = {0};
  expr_prog_t *exprs[SHADE_MAX * 2];
  size_t nexprs = 0;
  config_parse_chain_doc(text, CFG.frame_focus, default_chrome, con.sh, &con.n,
                         chr.sh, &chr.n, exprs, &nexprs, err, errcap);
  if (err && errcap && err[0]) {
    for (size_t i = 0; i < nexprs; i++) expr_free(exprs[i]);
    return false;
  }

  /* Each program to the chain whose shader points at it. Walked rather than
   * assumed: freeing one that the other chain still uses is the bug here that
   * would not show up until the next repaint. */
  for (size_t i = 0; i < nexprs; i++) {
    bool mine = false;
    for (size_t j = 0; j < chr.n && !mine; j++)
      if (chr.sh[j].amount_expr == exprs[i]) {
        chr.exprs[chr.nexprs++] = exprs[i];
        mine = true;
      }
    for (size_t j = 0; j < con.n && !mine; j++)
      if (con.sh[j].amount_expr == exprs[i]) {
        con.exprs[con.nexprs++] = exprs[i];
        mine = true;
      }
    if (!mine) expr_free(exprs[i]);
  }

  chain_clear(&n->chrome_chain);
  chain_clear(&n->content_chain);
  n->chrome_chain = chr;
  n->content_chain = con;
  pane_touch(n->pane);
  if (nchrome) *nchrome = chr.n;
  if (ncontent) *ncontent = con.n;
  return true;
}

/* The session's opinion about this pane at this moment, as a shader.
 *
 * Derived every frame rather than attached and remembered. Focus and drags
 * move through too many paths — hover, click, the finder, a close, a layout —
 * to keep an attachment in sync with, and a pane left grey by the one path
 * that forgot to clear it is a bug that only shows up in front of someone.
 * The rect and the collapsed flag are recomputed every pass for exactly this
 * reason; this is the same rule applied to colour. */
/* Which state this pane is in, or PSTATE_COUNT for none.
 *
 * A pane is usually in several at once, so the order here is the answer: the
 * first that matches wins and the rest are not asked. It runs from the most
 * transient and deliberate to the most ambient, because a pane you are holding
 * should not be recoloured by anything, a mode the whole screen is in outranks
 * a hint about one pane, and "not focused" is the weakest thing that can be
 * true of a pane. */
static pane_state_t pane_state_of(app_t *a, node_t *n) {
  /* Only once the pointer has actually moved: a press that turns out to be a
   * click would otherwise flash the whole session on its way to nothing. */
  if (a->drag.kind == DRAG_TITLE && a->drag.moved) {
    if (n->id == a->drag.src) return PSTATE_DRAGGING;
    if (n->id == a->drag.target) return PSTATE_DROP_HOVER;
    return PSTATE_DROP_TARGET;
  }
  if (!pane_alive(n->pane) && !pane_suspended(n->pane)) return PSTATE_DEAD;
  if (pane_suspended(n->pane)) return PSTATE_SUSPENDED;
  /* A rung pane outranks a scrolled or unfocused one, and *replaces* it rather
   * than stacking with it: states do not stack (D13), and of the things true
   * of a pane that rang while you were elsewhere, the ringing is the one you
   * needed telling. The focused pane never reaches here with a bell — looking
   * at it answers the bell before anything is drawn. */
  if (pane_bell(n->pane)) return PSTATE_BELL;
  if (pane_scrolled(n->pane)) return PSTATE_SCROLLED;
  if (n != cur(a)->focus) return PSTATE_UNFOCUSED;
  return PSTATE_COUNT;
}

/* ...and when it last became that, which is the part a frame cannot tell you.
 * Stamped here, in the one place that decides the state, so `since` cannot end
 * up describing a state the pane is no longer in. Idempotent within a frame:
 * the content pass and the chrome pass both ask, and the second sees no
 * change. */
static pane_state_t pane_state(app_t *a, node_t *n) {
  pane_state_t st = pane_state_of(a, n);
  if (st != n->last_state || !n->state_since) {
    n->last_state = st;
    n->state_since = now_ms_();
  }
  return st;
}

static size_t policy_shaders(app_t *a, node_t *n, shader_t *out, size_t cap) {
  pane_state_t st = pane_state(a, n);
  if (st >= PSTATE_COUNT) return 0;
  size_t k = CFG.state_n[st];
  if (k > cap) k = cap;
  for (size_t i = 0; i < k; i++) out[i] = CFG.state_shaders[st][i];
  return k;
}

static size_t chrome_policy_shaders(app_t *a, node_t *n, shader_t *out,
                                    size_t cap) {
  pane_state_t st = pane_state(a, n);
  if (st >= PSTATE_COUNT) return 0;
  size_t k = CFG.chrome_state_n[st];
  if (k > cap) k = cap;
  for (size_t i = 0; i < k; i++) out[i] = CFG.chrome_state_shaders[st][i];
  return k;
}

/* Does anything in this chain read the clock? If so the session has to keep
 * painting on its own, because the thing that will change next is the time.
 * Asked of the chain that was actually applied rather than of the config: a
 * pulse hung off `dead` costs a frame clock while a pane is dead and nothing
 * for the rest of the session. */
static void note_animation(app_t *a, const shader_t *chain, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (chain[i].amount_expr &&
        (expr_deps(chain[i].amount_expr) & EXPR_DEP_TIME)) {
      a->animating = true;
      return;
    }
}

/* A pane with nothing to apply costs nothing: no context is built and no cell
 * is visited, so an unshaded session emits the same bytes it did before. */
static void shade_leaf(app_t *a, screen_t *s, node_t *n) {
  shader_t chain[SHADE_CHAIN_MAX];
  size_t nc = 0;

  /* Configured first, then this pane's own, then policy last: what you asked
   * every pane to look like, adjusted for this pane, and only then the
   * session's opinion about this moment — which has to be able to grey out
   * whatever the other two produced. */
  for (size_t i = 0; i < CFG.nshaders && nc < SHADE_CHAIN_MAX; i++)
    chain[nc++] = CFG.shaders[i];
  for (size_t i = 0; i < n->content_chain.n && nc < SHADE_CHAIN_MAX; i++)
    chain[nc++] = n->content_chain.sh[i];
  nc += policy_shaders(a, n, &chain[nc], SHADE_CHAIN_MAX - nc);

  if (!nc) return;
  bool focused = n == cur(a)->focus;
  /* After policy_shaders, which is what stamps the transition. */
  int64_t now = now_ms_();
  shade_ctx_t base = {
      .now_ms = now,
      .state_ms = n->state_since ? now - n->state_since : 0,
      .focused = focused,
      .default_fg = CFG.default_fg,
      .default_bg = CFG.default_bg,
  };
  /* The screen's cursor belongs to whichever pane is focused, so it is only
   * this pane's cursor when this pane is the focused one. Handing it to any
   * other pane would have its spotlight chasing a cursor somewhere else. */
  if (focused && s->cursor_visible && s->cursor_x >= n->content.x &&
      s->cursor_y >= n->content.y) {
    base.has_cursor = true;
    base.cursor_x = (uint16_t)(s->cursor_x - n->content.x);
    base.cursor_y = (uint16_t)(s->cursor_y - n->content.y);
  }
  note_animation(a, chain, nc);
  shade_apply(s, chain, nc, n->content, NULL, &base);
}

/* Chrome: a pass over `r`, skipping `hole` (the contents, when the thing being
 * drawn has any). One pass over the whole frame rather than one per side, so
 * that an effect can travel round it — four passes would each count from zero
 * and a sweep would restart at every corner.
 *
 * The rect is passed in rather than taken from the node because the two things
 * that wear chrome are not the same shape: an open pane is its rect minus its
 * contents, and a collapsed pane is one row that is chrome all the way through
 * — its `content` is the size the program inside still believes it has, which
 * is somewhere else entirely, and punching that out of the row would remove
 * cells the row does not even overlap. `n` is only asked which state it is in,
 * so a collapsed row is coloured by the pane it stands for.
 *
 * No cursor. The cursor is in the contents by construction, so a frame pass
 * carrying one would hand an effect a position outside its own rect; `cursor`
 * reads 0 here, which is the truth about a frame. */
static void shade_chrome(app_t *a, screen_t *s, node_t *n, rect_t r,
                         const rect_t *hole) {
  shader_t chain[SHADE_CHAIN_MAX];
  size_t nc = 0;

  /* Configured, then this pane's own, then policy -- the same order the
   * contents use, and for the same reason: the session's opinion about this
   * moment has to be able to grey out whatever the other two produced. */
  for (size_t i = 0; i < CFG.nchrome_shaders && nc < SHADE_CHAIN_MAX; i++)
    chain[nc++] = CFG.chrome_shaders[i];
  for (size_t i = 0; i < n->chrome_chain.n && nc < SHADE_CHAIN_MAX; i++)
    chain[nc++] = n->chrome_chain.sh[i];
  nc += chrome_policy_shaders(a, n, &chain[nc], SHADE_CHAIN_MAX - nc);
  if (!nc) return;

  int64_t now = now_ms_();
  shade_ctx_t base = {
      .now_ms = now,
      .state_ms = n->state_since ? now - n->state_since : 0,
      .focused = n == cur(a)->focus,
      .default_fg = CFG.default_fg,
      .default_bg = CFG.default_bg,
  };
  note_animation(a, chain, nc);
  shade_apply(s, chain, nc, r, hole, &base);
}

/* Drawn after the panes, so the corner's hit is registered after the two gap
 * hits it sits on and wins those cells: the cross is one target, not the
 * overlap of two. */
static void draw_corners(app_t *a, screen_t *s) {
  for (size_t i = 0; i < a->ncorners; i++) {
    corner_t *c = &a->corners[i];
    char action[48];
    snprintf(action, sizeof action, "corner:%zu", i);
    hit_add(&s->hits, c->r.x, c->r.y, c->r.w, c->r.h, action);

    bool active = a->drag.kind == DRAG_EDGE && (a->drag.c_nh || a->drag.c_nv) &&
                  a->drag.c_nh && c->nh && a->drag.c_h[0] == c->h_id[0] &&
                  a->drag.c_hedge[0] == c->h_edge[0] &&
                  a->drag.c_v[0] == c->v_id[0];
    /* (C) The mark appears the moment the pointer is on it, before the dwell
     * that arms the two boundaries. A crossing is two cells wide and gives no
     * other sign it is anything: something has to say it is there, and the
     * ghost costs nothing if you were only passing through. */
    bool over =
        a->drag.kind == DRAG_NONE && ptr_on(a, c->r.x, c->r.y, c->r.w, c->r.h);
    if (!active && !over) continue;
    bool armed = active || now_ms_() - a->ptr_still_since >= CFG.hover_delay_ms;
    for (uint16_t x = c->r.x; x < c->r.x + c->r.w; x++)
      screen_text(s, x, c->r.y,
                  armed ? (active ? "\u256c" : "\u253c") : "\u253c", RESIZE_C,
                  NO_COLOR, active ? ATTR_BOLD : 0);
  }
}

static void draw_node(app_t *a, screen_t *s, node_t *n);

static void draw_cb(node_t *n, void *ud) {
  struct draw *d = ud;
  char action[48];

  /* Painted in order, so the hit list resolves last-painted-wins: the frame
   * focuses, the content forwards the mouse, the button splits. */
  snprintf(action, sizeof action, "focus:%u", n->id);
  hit_add(&d->s->hits, n->rect.x, n->rect.y, n->rect.w, n->rect.h, action);

  snprintf(action, sizeof action, "pane:%u", n->id);
  hit_add(&d->s->hits, n->content.x, n->content.y, n->content.w, n->content.h,
          action);

  draw_frame(d->a, d->s, n);
  if (pane_suspended(n->pane)) {
    /* Falls through to the shader pass rather than returning: a pane that has
     * not started is a state you can want to colour, and its label is the only
     * content it has. */
    /* The pane exists, is laid out, and has run nothing. Say what it would. */
    const char *label = pane_label(n->pane);
    char line[256];
    snprintf(line, sizeof line, "press a key to run: %s",
             label && *label ? label : "shell");
    /* Truncate rather than vanish: a hint that only appears on wide panes is
     * worse than a clipped one, and the pane may be narrow precisely because
     * there are twelve of them. */
    size_t w = strlen(line);
    if (w > n->content.w) {
      w = n->content.w;
      if (w > 1) {
        line[w - 1] = 0;
        line[w - 2] =
            0xe2; /* fall back to a plain dot rather than a cut UTF-8 */
        line[w - 2] = '.';
      } else {
        line[w] = 0;
      }
    }
    screen_text(d->s, (uint16_t)(n->content.x + (n->content.w - w) / 2),
                (uint16_t)(n->content.y + n->content.h / 2), line, TITLE_IDLE,
                NO_COLOR, 0);
  } else {
    pane_compose(n->pane, d->s, n->content.x, n->content.y,
                 n == cur(d->a)->focus);
  }
  /* Between the contents and the chrome that goes over them: the frame was
   * painted before this and lies outside the content rect, and the split guide
   * is painted after, so it stays legible on top of a shaded pane.
   *
   * The chrome pass runs here too, and for the same reason: the frame it
   * recolours is already painted, and the guide, the resize hints and the
   * corners are painted after it and stay in their own colours. An affordance
   * is not decoration and must not be dimmed along with the thing it is
   * offered on. */
  shade_leaf(d->a, d->s, n);
  shade_chrome(d->a, d->s, n, n->rect, &n->content);
  draw_split_guide(d->a, d->s, n);
}

struct bellsearch {
  bool found;
};
static void bell_cb(node_t *n, void *ud) {
  struct bellsearch *b = ud;
  if (pane_bell(n->pane)) b->found = true;
}
static bool tab_has_bell(tab_t *t) {
  struct bellsearch b = {false};
  walk(t->root, bell_cb, &b);
  return b.found;
}

static void draw_tab_strip(app_t *a, screen_t *s) {
  /* A pane is being carried: the strip is a row of destinations for as long as
   * that is true. Worked out once rather than per tab. */
  bool dragging_pane = a->drag.kind == DRAG_TITLE && a->drag.moved;
  uint16_t x = CFG.status_pad;
  uint16_t y = CFG.gap;

  /* Right side first, so a long tab list can never eat the indicators — the
   * same budgeting rule as the split button and the OSC buttons. */
  uint16_t right =
      (uint16_t)(s->cols > CFG.status_pad ? s->cols - CFG.status_pad : s->cols);
  char info[64];
  size_t np = app_pane_count(a);
  snprintf(info, sizeof info, "%zu pane%s", np, np == 1 ? "" : "s");
  uint16_t iw = (uint16_t)strlen(info);
  if (right > iw + 4) {
    screen_text(s, (uint16_t)(right - iw), y, info, TAB_COUNT, NO_COLOR, 0);
    right = (uint16_t)(right - iw - 1);
  }
  if (a->prefix) { /* the prefix is a mode: say so, and say which key */
    /* Rendered from the binding rather than written out, because it was
     * written out: the badge said "C-a" whatever you had configured, which is
     * the one place a rebound prefix could still lie to you. Measured too --
     * a prefix is not always three columns wide (`C-space`, `M-\``). */
    char pfx[24];
    config_chord_name(CFG.prefix_key, CFG.prefix_mods, pfx, sizeof pfx);
    uint16_t pw = cells(pfx);
    if (right > pw + 2) {
      screen_text(s, (uint16_t)(right - pw), y, pfx, PREFIX_FG, PREFIX_BG,
                  ATTR_BOLD);
      right = (uint16_t)(right - pw - 1);
    }
  }

  for (size_t i = 0; i < a->ntabs && x < right; i++) {
    tab_t *t = &a->tabs[i];
    char label[80];
    const char *nm = t->name[0] ? t->name : (t->purpose[0] ? t->purpose : "");
    /* A pane that rang in a tab you are not looking at is invisible without
     * this, and that is the case the whole indicator exists for. */
    bool rang = CFG.bell_indicator && tab_has_bell(t);
    if (nm[0])
      snprintf(label, sizeof label, " %zu:%s ", i + 1, nm);
    else
      snprintf(label, sizeof label, " %zu ", i + 1);

    /* Renaming: the tab's own cell becomes the editor, in the editor's
     * colours, so a half-typed name can never be mistaken for the tab's real
     * one. The caret is part of the label, so the width below — and therefore
     * the hit — is the width of what is actually drawn. */
    bool editing = a->renaming == RENAME_TAB && a->rename_id == t->id;
    if (editing) snprintf(label, sizeof label, " %s\u2588 ", a->rename_buf);

    bool active = i == a->cur;
    uint16_t attrs = active ? ATTR_BOLD : 0;
    /* Two independent signals: weight says which tab you are in, colour says
     * where the pointer is. Drawn once to learn the width, then again in the
     * hover colour if that width turns out to be under the pointer — which
     * costs a repaint of a few cells and guarantees the lit cells are the
     * registered ones. */
    uint16_t w =
        screen_text(s, x, y, label,
                    editing ? RENAME_FG : (active ? TAB_ACTIVE_FG : TAB_IDLE),
                    editing ? RENAME_BG : (active ? TAB_ACTIVE_BG : NO_COLOR),
                    editing ? ATTR_BOLD : attrs);
    /* Hovering keeps the active tab's fill — it is still the tab you are in —
     * so its feedback lands on the text instead. An inactive tab has no fill
     * to keep, and brightens. */
    /* The bell keeps its own colour here too, rather than taking the tab's —
     * an indicator drawn in the same dim grey as the label it sits next to is
     * an indicator you have to already be looking for. */
    if (rang) {
      char mark[24];
      snprintf(mark, sizeof mark, "%s ", CFG.bell_mark);
      w = (uint16_t)(w + screen_text(s, (uint16_t)(x + w), y, mark, BELL_C,
                                     active ? TAB_ACTIVE_BG : NO_COLOR,
                                     ATTR_BOLD));
    }
    if (!editing && ptr_on(a, x, y, w, 1))
      screen_text(s, x, y, label, active ? TAB_ACTIVE_HOVER_FG : TAB_HOVER,
                  active ? TAB_ACTIVE_BG : NO_COLOR, attrs | ATTR_BOLD);

    /* While a pane is in your hand, every tab it does not already live in is
     * somewhere it could go, and `ptr_on` says nothing during a drag by design --
     * so the strip has to draw the drop states itself. Same two states the panes
     * use: all the candidates in the drop colour, and the one under the pointer
     * filled, because these are all targets and that is the one you are on. */
    if (dragging_pane) {
      node_t *held = pane_by_id(a, a->drag.src);
      bool mine = held && tab_of(a, held) == i;
      if (!mine) {
        bool on = a->drag.tab_target == t->id;
        screen_text(s, x, y, label, on ? TAB_ACTIVE_HOVER_FG : DROP_C,
                    on ? DROP_C : NO_COLOR, attrs | ATTR_BOLD);
      }
    }
    char action[48];
    snprintf(action, sizeof action, "tab:%u", t->id);
    hit_add(&s->hits, x, y, w, 1, action);
    x = (uint16_t)(x + w);
  }

  /* A bare mark, spaced like the frame's own buttons rather than spelled out.
   * It used to read "+tab", because a pane frame carried a "+" for splitting
   * and two verbs that look identical is a UI bug the fork shipped. That "+"
   * went when the border became the button, so the collision it was avoiding
   * no longer exists and the word was left explaining itself to nobody.
   *
   * Padded to three cells for the same reason the frame's buttons are: a
   * one-cell target is a thing you miss with a mouse. What it does is said by
   * the hint under the pointer, which is where every other one-character
   * affordance here says it. */
  {
    char btn[24];
    snprintf(btn, sizeof btn, " %s ", CFG.newtab_mark);
    uint16_t bw = (uint16_t)(cells(CFG.newtab_mark) + 2);
    if (bw > 2 && x + bw <= right) {
      uint16_t w = screen_text(s, x, y, btn, TAB_IDLE, NO_COLOR, 0);
      if (ptr_on(a, x, y, w, 1))
        screen_text(s, x, y, btn, TAB_HOVER, NO_COLOR, ATTR_BOLD);
      /* The button that makes a tab is also a place to drop a pane into one. */
      if (dragging_pane)
        screen_text(s, x, y, btn,
                    a->drag.new_tab_target ? TAB_ACTIVE_HOVER_FG : DROP_C,
                    a->drag.new_tab_target ? DROP_C : NO_COLOR, ATTR_BOLD);
      hit_add(&s->hits, x, y, w, 1, "newtab");
    }
  }
}

/* The space between two of a split's children, or false if they are flush.
 * One implementation, because the drawing, the hit and the corner-finding all
 * have to agree about where a gap is down to the cell. */
static bool gap_rect(node_t *n, size_t i, rect_t *out) {
  if (!n || n->kind != NODE_SPLIT || i + 1 >= n->nkids) return false;
  rect_t a_r = n->kids[i]->rect, b_r = n->kids[i + 1]->rect;
  if (!a_r.w || !b_r.w) return false;
  if (n->dir == SPLIT_COLS) {
    uint16_t x0 = (uint16_t)(a_r.x + a_r.w);
    if (b_r.x <= x0) return false;
    *out = (rect_t){x0, a_r.y, (uint16_t)(b_r.x - x0), a_r.h};
  } else {
    uint16_t y0 = (uint16_t)(a_r.y + a_r.h);
    if (b_r.y <= y0) return false;
    *out = (rect_t){a_r.x, y0, a_r.w, (uint16_t)(b_r.y - y0)};
  }
  return true;
}

struct gapinfo {
  node_t *sp;
  size_t i;
  rect_t r;
};

static size_t collect_gaps(node_t *n, struct gapinfo *out, size_t cap,
                           size_t k) {
  if (!n || n->kind != NODE_SPLIT || k >= cap) return k;
  for (size_t i = 0; i + 1 < n->nkids && k < cap; i++) {
    rect_t g;
    if (!gap_rect(n, i, &g)) continue;
    out[k].sp = n;
    out[k].i = i;
    out[k].r = g;
    k++;
  }
  for (size_t i = 0; i < n->nkids; i++)
    k = collect_gaps(n->kids[i], out, cap, k);
  return k;
}

/* Where a boundary between rows crosses a boundary between columns.
 *
 * The two never overlap in the tree — a column boundary stops where the row
 * boundary begins, because they belong to different splits — so a corner is
 * found by adjacency rather than by intersection: the cells of the row gap
 * that have a column gap running into them from above or below.
 *
 * Both are collected when both are there. In a two-by-two the column boundary
 * above and the one below are separate splits that happen to line up, and
 * moving one without the other would leave a step in what reads as one line. */
static void find_corners(app_t *a) {
  a->ncorners = 0;
  if (!a->ntabs || !cur(a)->root) return;

  struct gapinfo g[64];
  size_t n = collect_gaps(cur(a)->root, g, 64, 0);

  /* Every place a column boundary meets a row boundary.
   *
   * They never overlap: whichever way the tree is nested, one of them stops
   * where the other begins. Which one stops depends on the nesting — rows of
   * columns give a full-width row boundary with column boundaries running into
   * it from above and below, columns of rows give a full-height column
   * boundary with row boundaries running into it from the sides — so the test
   * is that the two touch at all, in either axis, rather than one particular
   * arrangement of them.
   *
   * The crossing is always the column boundary's columns by the row
   * boundary's rows, and boundaries are grouped by the crossing they land on.
   * Two that line up are one crossing that moves both, because moving one of a
   * matched pair would put a step in what reads as a single line; two that do
   * not are separate crossings. */
  for (size_t i = 0; i < n; i++) {
    if (g[i].sp->dir != SPLIT_ROWS) continue;
    rect_t h = g[i].r;
    for (size_t j = 0; j < n; j++) {
      if (g[j].sp->dir != SPLIT_COLS) continue;
      rect_t v = g[j].r;

      bool touch_y =
          v.y <= (uint16_t)(h.y + h.h) && h.y <= (uint16_t)(v.y + v.h);
      bool touch_x =
          h.x <= (uint16_t)(v.x + v.w) && v.x <= (uint16_t)(h.x + h.w);
      if (!touch_y || !touch_x) continue;

      rect_t cr = {v.x, h.y, v.w, h.h};
      corner_t *c = NULL;
      for (size_t k = 0; k < a->ncorners; k++) {
        rect_t e = a->corners[k].r;
        if (e.x == cr.x && e.y == cr.y && e.w == cr.w && e.h == cr.h) {
          c = &a->corners[k];
          break;
        }
      }
      if (!c) {
        if (a->ncorners >= 16) break;
        c = &a->corners[a->ncorners++];
        *c = (corner_t){0};
        c->r = cr;
      }

      bool have = false;
      for (size_t k = 0; k < c->nh; k++)
        if (c->h_id[k] == g[i].sp->id && c->h_edge[k] == g[i].i) have = true;
      if (!have && c->nh < 2) {
        c->h_id[c->nh] = g[i].sp->id;
        c->h_edge[c->nh] = g[i].i;
        c->nh++;
      }
      have = false;
      for (size_t k = 0; k < c->nv; k++)
        if (c->v_id[k] == g[j].sp->id && c->v_edge[k] == g[j].i) have = true;
      if (!have && c->nv < 2) {
        c->v_id[c->nv] = g[j].sp->id;
        c->v_edge[c->nv] = g[j].i;
        c->nv++;
      }
    }
  }
}

static int corner_at(app_t *a, uint16_t x, uint16_t y) {
  for (size_t i = 0; i < a->ncorners; i++) {
    rect_t r = a->corners[i].r;
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return (int)i;
  }
  return -1;
}

static bool corner_uses(const corner_t *c, uint32_t id, size_t edge) {
  for (size_t i = 0; i < c->nh; i++)
    if (c->h_id[i] == id && c->h_edge[i] == edge) return true;
  for (size_t i = 0; i < c->nv; i++)
    if (c->v_id[i] == id && c->v_edge[i] == edge) return true;
  return false;
}

/* The gap between two panes is a handle, and nothing about two blank columns
 * says so. So it says it on hover — and says something else once you have hold
 * of it, because "you could move this" and "you are moving this" are different
 * claims and the second one should not have to be inferred from the panes
 * changing size.
 *
 * Dotted while available, doubled and bold while engaged: the same grammar the
 * split guide already uses, where dashes are a possibility and weight is a
 * commitment. */
static void draw_resize_hint(app_t *a, screen_t *s, node_t *split, size_t idx,
                             rect_t gapr) {
  bool active = a->drag.kind == DRAG_EDGE && a->drag.src == split->id &&
                a->drag.edge == idx;
  /* A corner moves two boundaries, so resting on it arms both of them: the
   * hint has to show what is about to move, not where the pointer is. */
  int ci = -1;
  if (a->drag.kind == DRAG_EDGE && (a->drag.c_nv || a->drag.c_nh)) {
    for (size_t k = 0; k < a->drag.c_nh; k++)
      if (a->drag.c_h[k] == split->id && a->drag.c_hedge[k] == idx)
        active = true;
    for (size_t k = 0; k < a->drag.c_nv; k++)
      if (a->drag.c_v[k] == split->id && a->drag.c_vedge[k] == idx)
        active = true;
  } else if (a->drag.kind == DRAG_NONE) {
    ci = corner_at(a, a->ptr_x, a->ptr_y);
  }
  if (ci >= 0 && (size_t)ci < a->ncorners &&
      corner_uses(&a->corners[ci], split->id, idx)) {
    if (false)
      active = true;
    else if (now_ms_() - a->ptr_still_since >= CFG.hover_delay_ms) {
      uint16_t attrs0 = 0;
      if (split->dir == SPLIT_COLS) {
        uint16_t x = (uint16_t)(gapr.x + gapr.w / 2);
        for (uint16_t y = gapr.y; y < gapr.y + gapr.h; y++)
          screen_text(s, x, y, "\u250a", RESIZE_C, NO_COLOR, attrs0);
      } else {
        uint16_t y = (uint16_t)(gapr.y + gapr.h / 2);
        for (uint16_t x = gapr.x; x < gapr.x + gapr.w; x++)
          screen_text(s, x, y, "\u2508", RESIZE_C, NO_COLOR, attrs0);
      }
      return;
    }
  }
  if (!active) {
    /* A pointer busy with anything else is not shopping for a boundary. */
    if (a->drag.kind != DRAG_NONE || !a->ptr_valid) return;
    /* Tested against the very rect just registered as this gap's hit, in the
     * same loop iteration — so the hint cannot appear anywhere the click would
     * not land, without having to trust a lookup to agree. */
    if (a->ptr_x < gapr.x || a->ptr_x >= gapr.x + gapr.w || a->ptr_y < gapr.y ||
        a->ptr_y >= gapr.y + gapr.h)
      return;
    /* Arm on dwell, like the split guide: crossing a gap on the way to a pane
     * is the most ordinary mouse movement there is. */
    if (now_ms_() - a->ptr_still_since < CFG.hover_delay_ms) return;
  }

  uint16_t attrs = active ? ATTR_BOLD : 0;

  /* An arrow in the middle of the line, naming the verb: the dots say this is
   * a handle, the arrow says what pulling it does. It points along the axis the
   * boundary *moves*, not the one it lies on — a divider between two columns is
   * drawn vertically and travels sideways, and the useful half of that is the
   * travelling. Always bold: it is the one cell meant to be read.
   *
   * Double arrows rather than U+2194/U+2195, which carry emoji presentation and
   * are widely drawn double-width; screen_text books a chrome cell as one, so a
   * glyph that drew as two would shift the row. They also happen to rhyme with
   * the doubled line the active state uses. */
  if (split->dir == SPLIT_COLS) {
    uint16_t x = (uint16_t)(gapr.x + gapr.w / 2);
    for (uint16_t y = gapr.y; y < gapr.y + gapr.h; y++)
      screen_text(s, x, y, active ? "\u2551" : "\u250a", RESIZE_C, NO_COLOR,
                  attrs);
    screen_text(s, x, (uint16_t)(gapr.y + gapr.h / 2), "\u21d4", RESIZE_C,
                NO_COLOR, ATTR_BOLD);
  } else {
    uint16_t y = (uint16_t)(gapr.y + gapr.h / 2);
    for (uint16_t x = gapr.x; x < gapr.x + gapr.w; x++)
      screen_text(s, x, y, active ? "\u2550" : "\u2508", RESIZE_C, NO_COLOR,
                  attrs);
    screen_text(s, (uint16_t)(gapr.x + gapr.w / 2), y, "\u21d5", RESIZE_C,
                NO_COLOR, ATTR_BOLD);
  }
}

static void draw_node(app_t *a, screen_t *s, node_t *n) {
  if (n->collapsed) {
    draw_collapsed(a, s, n);
    return;
  }
  if (n->kind == NODE_LEAF) {
    struct draw d = {a, s};
    draw_cb(n, &d);
    return;
  }

  /* The gap between two children is the boundary you can drag. It is drawn as
   * nothing, but it is a real target, derived from the rects the children were
   * just given rather than recomputed from the config. */
  for (size_t i = 0; i + 1 < n->nkids; i++) {
    rect_t gapr;
    if (!gap_rect(n, i, &gapr)) continue;
    char action[48];
    snprintf(action, sizeof action, "edge:%u:%zu", n->id, i);
    hit_add(&s->hits, gapr.x, gapr.y, gapr.w, gapr.h, action);
    draw_resize_hint(a, s, n, i, gapr);
  }

  for (size_t i = 0; i < n->nkids; i++) draw_node(a, s, n->kids[i]);
}

/* ---- the pane finder ---------------------------------------------------- */

typedef struct {
  node_t *node;
  size_t tab;
} find_entry_t;

struct find_collect {
  app_t *a;
  const char *query;
  find_entry_t *out;
  size_t n, max;
};

static bool ci_contains(const char *hay, const char *needle) {
  if (!*needle) return true;
  size_t nl = strlen(needle);
  for (const char *p = hay; *p; p++) {
    size_t i = 0;
    while (i < nl && p[i] &&
           tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
      i++;
    if (i == nl) return true;
  }
  return false;
}

static void find_cb(node_t *n, void *ud) {
  struct find_collect *c = ud;
  if (c->n >= c->max) return;
  char hay[512];
  size_t ti = tab_of(c->a, n);
  snprintf(hay, sizeof hay, "%s %s %s", pane_title(n->pane), n->purpose,
           ti == (size_t)-1 ? "" : c->a->tabs[ti].name);
  if (!ci_contains(hay, c->query)) return;
  c->out[c->n].node = n;
  c->out[c->n].tab = ti;
  c->n++;
}

static size_t finder_entries(app_t *a, find_entry_t *out, size_t max) {
  struct find_collect c = {a, a->query, out, 0, max};
  walk_all(a, find_cb, &c);
  return c.n;
}

/* The cheatsheet: every binding the config has, drawn as a pane that happens
 * to float.
 *
 * Built from CFG.binds rather than from a list of what the defaults are, so a
 * rebound key shows the key you actually bound and a key you removed is not
 * advertised. Bindings that share an action are merged onto one row — `h` and
 * `left` are one thing to learn, not two — and the nine tab digits are one
 * row rather than nine.
 */
typedef struct {
  const char *group; /* NULL for a heading, or for the blank row above one */
  char chord[40];
  const char *label; /* NULL for the blank row */
} help_row_t;

static size_t help_rows(help_row_t *out, size_t cap) {
  static const char *const GROUPS[] = {"panes",    "focus",  "size",   "tabs",
                                       "projects", "scroll", "session"};
  size_t n = 0;
  for (size_t g = 0; g < sizeof GROUPS / sizeof *GROUPS && n < cap; g++) {
    bool titled = false;
    /* Actions in the order the label table lists them, so the sheet reads the
     * same whatever order a config happened to bind things in. */
    for (int act = ACT_NONE + 1; act <= ACT_SELECT_TAB_1 && n < cap; act++) {
      const char *group = config_action_group((action_t)act);
      const char *label = config_action_label((action_t)act);
      if (!group || !label || strcmp(group, GROUPS[g]) != 0) continue;

      char chords[40] = {0};
      size_t used = 0;
      for (size_t i = 0; i < CFG.nbinds; i++) {
        if (CFG.binds[i].direct) continue; /* listed on their own, below */
        action_t bound = CFG.binds[i].action;
        /* The nine tab digits are one row: nine of them is a table, not a
         * thing to learn. */
        bool tabish = act == ACT_SELECT_TAB_1 && bound >= ACT_SELECT_TAB_1;
        if (bound != (action_t)act && !tabish) continue;
        if (tabish && bound != ACT_SELECT_TAB_1 &&
            bound != ACT_SELECT_TAB_1 + 8)
          continue; /* first and last, shown as a range */

        char one[24];
        config_chord_name(CFG.binds[i].key, CFG.binds[i].mods, one, sizeof one);
        int wrote = snprintf(chords + used, sizeof chords - used, "%s%s",
                             used ? (tabish ? "\u2026" : " ") : "", one);
        if (wrote > 0) used += (size_t)wrote;
        if (used >= sizeof chords - 8) break;
      }
      if (!chords[0]) continue; /* bound to nothing: do not advertise it */

      if (!titled) {
        /* A blank line above every heading but the first: the caption at the
         * top already separates that one, and a sheet whose sections run into
         * each other is a wall of rows to scan rather than five short lists. */
        if (n && n < cap) out[n++] = (help_row_t){NULL, {0}, NULL};
        out[n++] = (help_row_t){NULL, {0}, GROUPS[g]};
        titled = true;
        if (n >= cap) break;
      }
      out[n] = (help_row_t){GROUPS[g], {0}, label};
      snprintf(out[n].chord, sizeof out[n].chord, "%s", chords);
      n++;
    }
  }

  /* Bindings that need no leader, in their own section because the caption at
   * the top of the sheet says "<prefix> then:" and these are the ones that is
   * not true of. Listed in the order they were bound rather than by action:
   * they are a short, deliberate list somebody wrote by hand. */
  if (config_has_direct(&CFG) && n < cap) {
    if (n && n < cap) out[n++] = (help_row_t){NULL, {0}, NULL};
    out[n++] = (help_row_t){NULL, {0}, "without the leader"};
    for (size_t i = 0; i < CFG.nbinds && n < cap; i++) {
      if (!CFG.binds[i].direct) continue;
      const char *label = config_action_label(CFG.binds[i].action);
      if (!label) continue; /* unbound with `none`, or nothing to say */
      out[n] = (help_row_t){"direct", {0}, label};
      config_chord_name(CFG.binds[i].key, CFG.binds[i].mods, out[n].chord,
                        sizeof out[n].chord);
      n++;
    }
  }
  return n;
}

/* ---- modals -------------------------------------------------------------
 *
 * A surface that floats over the layout: everything behind it is pushed back,
 * it is opaque, it wears a pane's frame so it plainly belongs to this program,
 * and it owns the topmost hits while it is up. The cheatsheet is the first
 * one; the shape is here rather than inside it so the second one costs a
 * function call and looks the same.
 */

/* Push the whole screen back, so what floats reads as being in front.
 *
 * This is the shader pass, aimed at the screen instead of a pane: the same
 * dim, the same materialisation of "terminal default" colours (without which
 * the chrome would not darken at all, since most of it has no colour of its
 * own). Nothing about it is special-cased, which is the nice part -- a scrim
 * is just a colour pass with everything in its rect. */
static void draw_scrim(app_t *a, screen_t *s) {
  if (!CFG.modal_scrim) return;
  shader_t dim;
  if (!shader_make(&dim, "dim", (color_t){0}, CFG.modal_scrim)) return;
  shade_ctx_t base = {
      .now_ms = now_ms_(),
      .default_fg = CFG.default_fg,
      .default_bg = CFG.default_bg,
  };
  shade_apply(s, &dim, 1, (rect_t){0, 0, s->cols, s->rows}, NULL, &base);
}

/* The frame every modal wears: an opaque box with a pane's corners, a title
 * in the top rule and a close button where a pane keeps one. Returns the rect
 * *inside* it, which is all a caller should care about. */
static rect_t modal_frame(app_t *a, screen_t *s, uint16_t w, uint16_t h,
                          const char *title, const char *close_action) {
  if (w > s->cols) w = s->cols;
  if (h > s->rows) h = s->rows;
  uint16_t x = (uint16_t)((s->cols - w) / 2), y = (uint16_t)((s->rows - h) / 2);
  uint16_t x1 = (uint16_t)(x + w - 1), y1 = (uint16_t)(y + h - 1);

  /* Opaque, or the panes underneath read through it: a modal you can see a
   * shell prompt through is a modal nobody trusts. */
  for (uint16_t yy = y; yy < y + h; yy++)
    for (uint16_t xx = x; xx < x + w; xx++)
      screen_text(s, xx, yy, " ", NO_COLOR, MODAL_BG, 0);

  const char *tl = CFG.rounded ? "\u256d" : "\u250c",
             *tr = CFG.rounded ? "\u256e" : "\u2510";
  const char *bl = CFG.rounded ? "\u2570" : "\u2514",
             *br = CFG.rounded ? "\u256f" : "\u2518";
  screen_text(s, x, y, tl, MODAL_BORDER, MODAL_BG, 0);
  screen_text(s, x1, y, tr, MODAL_BORDER, MODAL_BG, 0);
  screen_text(s, x, y1, bl, MODAL_BORDER, MODAL_BG, 0);
  screen_text(s, x1, y1, br, MODAL_BORDER, MODAL_BG, 0);
  for (uint16_t xx = (uint16_t)(x + 1); xx < x1; xx++) {
    screen_text(s, xx, y, "\u2500", MODAL_BORDER, MODAL_BG, 0);
    screen_text(s, xx, y1, "\u2500", MODAL_BORDER, MODAL_BG, 0);
  }
  for (uint16_t yy = (uint16_t)(y + 1); yy < y1; yy++) {
    screen_text(s, x, yy, "\u2502", MODAL_BORDER, MODAL_BG, 0);
    screen_text(s, x1, yy, "\u2502", MODAL_BORDER, MODAL_BG, 0);
  }

  if (title && *title && w > cells(title) + 4) {
    char buf[80];
    snprintf(buf, sizeof buf, " %s ", title);
    screen_text(s, (uint16_t)(x + (w - cells(buf)) / 2), y, buf, MODAL_TITLE,
                MODAL_BG, ATTR_BOLD);
  }

  /* The close button a pane has, in the place a pane keeps it. Two cells of
   * target for a one-cell mark, for the reason the frame's own buttons are
   * three: a one-cell target is a thing you miss with a mouse. */
  if (close_action && w > 10) {
    uint16_t bw = (uint16_t)(cells(CFG.close_mark) + 1);
    uint16_t bx = (uint16_t)(x1 - bw);
    bool hot = ptr_on(a, bx, y, bw, 1);
    char cell[24];
    snprintf(cell, sizeof cell, "%s ", CFG.close_mark);
    screen_text(s, bx, y, cell, hot ? MODAL_BTN_HOVER : MODAL_BTN, MODAL_BG,
                hot ? ATTR_BOLD : 0);
    hit_add(&s->hits, bx, y, bw, 1, close_action);
  }

  return (rect_t){(uint16_t)(x + 1), (uint16_t)(y + 1), (uint16_t)(w - 2),
                  (uint16_t)(h - 2)};
}

/* Draw at most `limit` columns of `text`, cut on a character boundary.
 *
 * The box is clamped to the screen, so on a small terminal the rows are wider
 * than what is left of it -- and a label that runs on writes straight over
 * the frame it is supposed to be inside. */
static uint16_t help_text(screen_t *s, uint16_t x, uint16_t y, const char *text,
                          uint16_t limit, color_t fg, color_t bg,
                          uint16_t attrs) {
  if (!limit) return 0;
  char buf[256];
  size_t n = 0, seen = 0;
  for (const char *p = text; *p && n < sizeof buf - 1; p++) {
    if (((unsigned char)*p & 0xC0) != 0x80) { /* a new character starts here */
      if (seen == limit) break;
      seen++;
    }
    buf[n++] = *p;
  }
  buf[n] = 0;
  return screen_text(s, x, y, buf, fg, bg, attrs);
}

static void draw_help(app_t *a, screen_t *s) {
  help_row_t rows[64];
  size_t n = help_rows(rows, 64);
  if (!n) return;

  /* Two columns, split at a group boundary so a heading never ends a column
   * with nothing under it. One column when the screen is too narrow. */
  uint16_t cw[2] = {0, 0};
  uint16_t kw[2] = {0, 0}; /* the chord column, padded so labels line up */
  size_t split = n;
  bool two = s->cols >= 78;
  if (two) {
    size_t half = (n + 1) / 2;
    split = half;
    while (split < n && rows[split].group) split++; /* to the next heading */
    if (split >= n) {
      split = half;
      while (split > 1 && rows[split].group) split--;
    }
    /* Land on the heading itself, not on the blank row above it: a column
     * that starts with a blank line looks like a mistake. */
    if (split < n && !rows[split].group && !rows[split].label) split++;
  }
  for (size_t i = 0; i < n; i++) {
    uint16_t col = (two && i >= split) ? 1 : 0;
    if (rows[i].group && cells(rows[i].chord) > kw[col])
      kw[col] = cells(rows[i].chord);
  }
  for (size_t i = 0; i < n; i++) {
    if (!rows[i].label) continue; /* the blank row wants nothing */
    uint16_t col = (two && i >= split) ? 1 : 0;
    uint16_t want = (uint16_t)(kw[col] + 2 + cells(rows[i].label));
    if (!rows[i].group) want = (uint16_t)cells(rows[i].label);
    if (want > cw[col]) cw[col] = want;
  }

  uint16_t body = (uint16_t)(cw[0] + (two ? cw[1] + 4 : 0));
  size_t left_n = two ? split : n, right_n = two ? n - split : 0;
  /* The fold falls on a heading, which leaves the blank row above it at the
   * bottom of the first column: a row that draws nothing and would otherwise
   * make the box a line taller than it needs to be. */
  if (left_n && !rows[left_n - 1].label) left_n--;
  if (right_n && !rows[n - 1].label) right_n--;
  uint16_t lines = (uint16_t)(left_n > right_n ? left_n : right_n);

  /* A terminal too short for the whole sheet gets as much of it as fits and a line
   * saying how much it did not. `modal_frame` clamps the box to the screen, so
   * without this the rows past the bottom are simply not drawn -- and a list that
   * quietly stops is worse than a short one, because the reader has no way to know
   * the key they are looking for is on it. */
  uint16_t room = (uint16_t)(s->rows > 7 ? s->rows - 7 : 1);
  uint16_t shown = lines > room ? room : lines;
  /* Counted, not calculated: the two columns are not the same length and a blank
   * spacer is not a row anybody is missing. Arithmetic on `lines` claimed six rows
   * were hidden on a screen showing all of them. */
  size_t hidden = 0;
  for (size_t i = 0; i < n; i++) {
    size_t row = (two && i >= split) ? i - split : i;
    if (row >= shown && rows[i].label) hidden++;
  }

  rect_t in = modal_frame(a, s, (uint16_t)(body + 6),
                          (uint16_t)(shown + 6 + (hidden ? 1 : 0)), "keys",
                          "closehelp");
  uint16_t x1 = (uint16_t)(in.x + in.w); /* the right border */

  /* Everything here is prefix-then-key, and saying so once beats repeating
   * the prefix on thirty rows. */
  char lead[80], pfx[24];
  config_chord_name(CFG.prefix_key, CFG.prefix_mods, pfx, sizeof pfx);
  snprintf(lead, sizeof lead, "%s then:", pfx);
  help_text(s, (uint16_t)(in.x + 2), (uint16_t)(in.y + 1), lead, in.w, HINT_C,
            MODAL_BG, 0);

  for (size_t i = 0; i < n; i++) {
    bool right = two && i >= split;
    size_t row = right ? i - split : i;
    if (row >= shown) continue; /* past the bottom: counted, not drawn */
    uint16_t ry = (uint16_t)(in.y + 3 + row);
    if (ry >= in.y + in.h) continue;
    if (!rows[i].label) continue; /* the blank row: it occupies, it draws not */
    uint16_t rx = (uint16_t)(in.x + 2 + (right ? cw[0] + 4 : 0));

    /* Everything is cut at the frame, and the left column additionally at the
     * right one, so a clamped box loses words rather than its border. */
    uint16_t room = (uint16_t)(x1 > rx ? x1 - rx : 0);
    if (two && !right && cw[0] + 2 < room) room = (uint16_t)(cw[0] + 2);

    if (!rows[i].group) {
      help_text(s, rx, ry, rows[i].label, room, MODAL_TITLE, MODAL_BG,
                ATTR_BOLD);
      continue;
    }
    help_text(s, rx, ry, rows[i].chord, room, MODAL_TITLE, MODAL_BG, 0);
    uint16_t lx = (uint16_t)(rx + kw[right ? 1 : 0] + 2);
    help_text(s, lx, ry, rows[i].label, (uint16_t)(x1 > lx ? x1 - lx : 0),
              MODAL_FG, MODAL_BG, 0);
  }

  /* Said inside the box, where the rows stopped, rather than left to be noticed. */
  if (hidden) {
    char more[64];
    snprintf(more, sizeof more, "+%zu more, in docs/keys.md", hidden);
    help_text(s, (uint16_t)(in.x + 2), (uint16_t)(in.y + 3 + shown), more, in.w,
              HINT_C, MODAL_BG, 0);
  }

  const char *foot = " any key closes this ";
  if (in.w > cells(foot))
    screen_text(s, (uint16_t)(in.x + (in.w - cells(foot)) / 2),
                (uint16_t)(in.y + in.h), foot, HINT_C, MODAL_BG, 0);
}

/* How many rows of results the box shows at once. Past this it scrolls, and
 * the point of a picker is that you narrow rather than scroll. */
#define FINDER_ROWS 10

/* One row of a picker.
 *
 * Three columns, because both pickers turned out to want the same three: a
 * dim thing on the left that says where it belongs (a tab, a group), the
 * thing itself in the middle, and a dim tail on the right that says something
 * useful about it (a purpose, the chord that would run it). */
typedef struct {
  char left[24];
  char mid[80];
  char right[80];
  bool here;       /* mark this row: the pane you are already in */
  char action[48]; /* what a click on it does */
} pick_row_t;

/* A band of background, for a row that is selected or under the pointer. The
 * whole width, not just the text: a highlight that stops where the words stop
 * reads as a highlighted *word* rather than a chosen row. */
static void fill_row(screen_t *s, uint16_t x, uint16_t y, uint16_t w,
                     color_t bg) {
  for (uint16_t i = 0; i < w; i++)
    screen_text(s, (uint16_t)(x + i), y, " ", NO_COLOR, bg, 0);
}

/* Fill `out` with the panes matching the query. */
static size_t finder_rows(app_t *a, pick_row_t *out, size_t max) {
  find_entry_t entries[64];
  size_t n = finder_entries(a, entries, max < 64 ? max : 64);
  for (size_t i = 0; i < n; i++) {
    find_entry_t *e = &entries[i];
    pick_row_t *r = &out[i];
    memset(r, 0, sizeof *r);
    /* A tab with no name is its number alone: "1:" with nothing after it
     * looks like something failed to load. */
    const char *tname = e->tab == (size_t)-1 ? "" : a->tabs[e->tab].name;
    if (tname && *tname)
      snprintf(r->left, sizeof r->left, "%zu:%.10s", e->tab + 1, tname);
    else
      snprintf(r->left, sizeof r->left, "%zu", e->tab + 1);
    const char *title = pane_title(e->node->pane);
    snprintf(r->mid, sizeof r->mid, "%s", title && *title ? title : "pane");
    snprintf(r->right, sizeof r->right, "%s", e->node->purpose);
    r->here = e->node == cur(a)->focus && e->tab == a->cur;
    snprintf(r->action, sizeof r->action, "find:%u", e->node->id);
  }
  return n;
}

/* ---- the command palette -------------------------------------------------
 *
 * Every action, by name, without having to know its key. The cheatsheet
 * answers "what is the key for this?" and needs you to then press it; this
 * answers "just do the thing", which is the question you have when you cannot
 * remember there was a key at all.
 *
 * It lists actions that are *not* bound too, which the cheatsheet cannot --
 * an unbound action is exactly the one you have no other way to reach. Where
 * a binding does exist it is shown on the row, so using the palette teaches
 * the key that would have skipped it.
 */
static size_t palette_rows(app_t *a, pick_row_t *out, size_t max) {
  size_t n = 0;
  for (int act = ACT_NONE + 1; act < ACT_SELECT_TAB_1 && n < max; act++) {
    const char *label = config_action_label((action_t)act);
    const char *group = config_action_group((action_t)act);
    const char *name = config_action_name((action_t)act);
    if (!label || !group || !name) continue;
    /* "send the prefix itself" means "type this key", which is not a command
     * you can ask for by name -- picking it from a list would send the prefix
     * to a pane you cannot see behind the list. */
    if (act == ACT_LITERAL_PREFIX) continue;

    /* The chord, if it has one, and the prefix it needs. A direct binding
     * carries no prefix, so saying so would be a lie about how to press it. */
    /* Every chord for it, the way the cheatsheet lists them: binding a key
     * in a config adds to the defaults rather than replacing them, so showing
     * only the first would name a key the user did not choose and hide the
     * one they did. A direct binding carries no prefix, and saying otherwise
     * would be a lie about how to press it. */
    char chord[48] = "";
    size_t used = 0;
    for (size_t i = 0; i < CFG.nbinds && used < sizeof chord - 12; i++) {
      if (CFG.binds[i].action != (action_t)act) continue;
      char c[24], pfx[24] = "";
      config_chord_name(CFG.binds[i].key, CFG.binds[i].mods, c, sizeof c);
      if (!CFG.binds[i].direct)
        config_chord_name(CFG.prefix_key, CFG.prefix_mods, pfx, sizeof pfx);
      int wrote = snprintf(chord + used, sizeof chord - used, "%s%s%s%s",
                           used ? " \u00b7 " : "", pfx, *pfx ? " " : "", c);
      if (wrote > 0) used += (size_t)wrote;
    }

    /* Matched on everything the row shows *and* on the name it has in a
     * config file, so "split-cols" finds it for someone who has been editing
     * the config and "split into columns" for someone who has not. */
    char hay[256];
    snprintf(hay, sizeof hay, "%s %s %s %s", label, group, name, chord);
    if (!ci_contains(hay, a->query)) continue;

    pick_row_t *r = &out[n++];
    memset(r, 0, sizeof *r);
    snprintf(r->left, sizeof r->left, "%s", group);
    snprintf(r->mid, sizeof r->mid, "%s", label);
    snprintf(r->right, sizeof r->right, "%s", chord);
    snprintf(r->action, sizeof r->action, "run:%d", act);
  }
  return n;
}

/* ---- the project picker --------------------------------------------------
 *
 * The third subject, and the one that answers a different question from the
 * other two: the finder searches what *exists* in this session, while this
 * lists what exists on *disk* -- including the projects with no workspace open
 * yet, which is most of them and the whole point.
 *
 * The scan happens once, when the picker opens, into a->projects. Not per frame:
 * draw_picker asks for its rows every frame, and a readdir of ~/dev at 120Hz
 * would be a filesystem walk per repaint. Once per opening is also exactly when
 * the answer has to be right, so nothing is remembered for longer than that.
 */
static size_t workspace_rows(app_t *a, pick_row_t *out, size_t max) {
  size_t n = 0;
  for (size_t i = 0; i < a->nprojects && n < max; i++) {
    const project_t *p = &a->projects[i];
    uint32_t tab = app_workspace_tab(a, p->slug);
    /* Matched on the name, the path and what kind it is, so `.git` narrows to
     * the ones with no layout yet -- which is the list you want when you are
     * about to write one. */
    char hay[640];
    snprintf(hay, sizeof hay, "%s %s %s", p->name, p->path,
             p->layout[0] ? PROJECT_LAYOUT_FILE : ".git");
    if (!ci_contains(hay, a->query)) continue;

    pick_row_t *r = &out[n++];
    memset(r, 0, sizeof *r);
    /* The root it came from rather than the whole path: which of `~/dev` and
     * `~/work` a project is in is the part that distinguishes two of the same
     * name, and the rest is the name again. */
    const char *slash = strrchr(p->path, '/');
    size_t keep = slash ? (size_t)(slash - p->path) : strlen(p->path);
    const char *root_end = p->path + keep;
    const char *root_start = root_end;
    while (root_start > p->path && root_start[-1] != '/') root_start--;
    snprintf(r->left, sizeof r->left, "%.*s", (int)(root_end - root_start),
             root_start);
    snprintf(r->mid, sizeof r->mid, "%s", p->name);
    /* What it is, or what it already is: an open workspace says how many panes
     * it has, and a project with no layout says so, which reads as the
     * invitation it is. */
    if (tab)
      snprintf(r->right, sizeof r->right, "%s", p->slug);
    else
      snprintf(r->right, sizeof r->right, "%s",
               p->layout[0] ? PROJECT_LAYOUT_FILE : ".git \u00b7 no layout");
    r->here = tab != 0 && tab == cur(a)->id;
    snprintf(r->action, sizeof r->action, "open:%zu", i);
  }
  return n;
}

static size_t picker_rows(app_t *a, pick_row_t *out, size_t max) {
  if (a->picker == PICK_PALETTE) return palette_rows(a, out, max);
  if (a->picker == PICK_WORKSPACES) return workspace_rows(a, out, max);
  return finder_rows(a, out, max);
}

static size_t picker_count(app_t *a) {
  pick_row_t rows[128];
  return picker_rows(a, rows, 128);
}

/* A picker is a modal, like the cheatsheet: scrim behind it, a pane's frame
 * around it, a title and a close button in the places a pane keeps them. The
 * finder used to be a bare coloured rectangle painted over whatever pane it
 * landed on, which read as output from the program underneath rather than as
 * a window in front of it -- and gave it no title, no way out you could see,
 * and no edge to tell you where it ended.
 *
 * It differs from the cheatsheet in the one way that matters: the cheatsheet
 * is *read*, so any key dismisses it, while this one is *used*, so it keeps
 * the keyboard until it is answered. */
static void draw_picker(app_t *a, screen_t *s) {
  pick_row_t rows[128];
  size_t n = picker_rows(a, rows, 128);
  if (a->sel >= n) a->sel = n ? n - 1 : 0;

  bool palette = a->picker == PICK_PALETTE;
  bool projects = a->picker == PICK_WORKSPACES;
  const char *title = palette ? "commands" : projects ? "projects" : "find";
  const char *close = palette    ? "closepalette"
                      : projects ? "closeprojects"
                                 : "closefind";

  /* The window of results on screen. Kept around the selection rather than
   * anchored at the top, so arrowing past the bottom scrolls instead of
   * moving the selection somewhere you cannot see it. */
  size_t shown = n > FINDER_ROWS ? FINDER_ROWS : n;
  size_t first = a->sel >= shown ? a->sel - shown + 1 : 0;

  uint16_t w = 64;
  if (w > s->cols - 4) w = (uint16_t)(s->cols > 24 ? s->cols - 4 : s->cols);
  /* border, query, blank, rows (at least one, for "no matches"), border */
  uint16_t list = (uint16_t)(shown ? shown : 1);
  uint16_t h = (uint16_t)(list + 5);
  if (h > s->rows) h = s->rows;

  rect_t in = modal_frame(a, s, w, h, title, close);

  /* The query, in a field that looks like one: a program you type into should
   * show you where the typing goes even when you have not typed anything. */
  uint16_t fy = (uint16_t)(in.y + 1);
  fill_row(s, in.x, fy, in.w, FINDER_BG);
  uint16_t qx = (uint16_t)(in.x + 1);
  qx += help_text(s, qx, fy, "\u203a ", in.w, HINT_C, FINDER_BG, 0);
  qx += help_text(s, qx, fy, a->query, (uint16_t)(in.x + in.w - qx), FINDER_FG,
                  FINDER_BG, ATTR_BOLD);
  if (qx < in.x + in.w)
    screen_text(s, qx, fy, "\u2588", FINDER_FG, FINDER_BG, 0);

  uint16_t ly = (uint16_t)(in.y + 3); /* the first result row */

  if (!n) {
    /* An empty box says the picker is broken; this says the query is. */
    const char *none =
        palette ? "no command matches that"
                : (a->query[0] ? "no pane matches that" : "no panes");
    help_text(s, (uint16_t)(in.x + 2), ly, none, (uint16_t)(in.w - 2), HINT_C,
              MODAL_BG, 0);
    return;
  }

  for (size_t i = 0; i < shown; i++) {
    size_t idx = first + i;
    if (idx >= n) break;
    pick_row_t *e = &rows[idx];
    uint16_t yy = (uint16_t)(ly + i);
    if (yy >= in.y + in.h) break;

    hit_add(&s->hits, in.x, yy, in.w, 1, e->action);

    /* Selected wins over hovered: the pointer may be resting anywhere, and
     * the row Enter would take is the one that has to be unambiguous. */
    bool on = idx == a->sel;
    bool hot = !on && ptr_on(a, in.x, yy, in.w, 1);
    color_t bg = on ? FINDER_SEL_BG : (hot ? FINDER_BG : MODAL_BG);
    color_t fg = on ? FINDER_SEL_FG : FINDER_FG;
    fill_row(s, in.x, yy, in.w, bg);

    /* Which pane you are in now, so the list says where you are as well as
     * where you could go. A different mark from the query prompt: two rows
     * that both begin with the same glyph invite you to read one as the
     * other. */
    uint16_t cx = (uint16_t)(in.x + 1);
    cx += help_text(s, cx, yy, e->here ? "\u2022" : " ", 1, fg, bg, 0);
    cx++;

    /* Three columns, drawn as three pieces rather than one printf, so the
     * part that matters can be told from the parts that qualify it. */
    uint16_t lw = 13;
    if (in.w > 40) {
      help_text(s, cx, yy, e->left, lw, on ? fg : HINT_C, bg, 0);
      cx = (uint16_t)(cx + lw);
    }

    uint16_t room = (uint16_t)(in.x + in.w > cx ? in.x + in.w - cx - 1 : 0);
    uint16_t mid_w = (uint16_t)(room > 20 ? (in.w > 56 ? 26 : 18) : room);
    uint16_t drew = help_text(s, cx, yy, e->mid, mid_w, fg, bg, ATTR_BOLD);
    cx = (uint16_t)(cx + (drew > mid_w ? drew : mid_w) + 1);

    if (*e->right && cx < in.x + in.w)
      help_text(s, cx, yy, e->right, (uint16_t)(in.x + in.w - cx - 1),
                on ? fg : HINT_C, bg, 0);
  }

  /* What is off the top and bottom of the window. Without this a list that
   * scrolls looks like a list that ends. */
  if (first)
    screen_text(s, (uint16_t)(in.x + in.w - 1), ly, "\u2191", HINT_C, MODAL_BG,
                0);
  if (first + shown < n)
    screen_text(s, (uint16_t)(in.x + in.w - 1), (uint16_t)(ly + shown - 1),
                "\u2193", HINT_C, MODAL_BG, 0);

  /* The count belongs where the eye already goes for "how much is there", and
   * it is the answer to "is my query too narrow, or is there nothing?". */
  char foot[64];
  snprintf(foot, sizeof foot, " %zu of %zu \u00b7 \u2191\u2193 enter ",
           a->sel + 1, n);
  if (in.w > cells(foot))
    screen_text(s, (uint16_t)(in.x + (in.w - cells(foot)) / 2),
                (uint16_t)(in.y + in.h), foot, HINT_C, MODAL_BG, 0);
}

/* Defined with the other action handling, further down: a picker row that
 * says "run:" means one of these, and running it is that code's business. */
/* Push the focused pane one tab along, and say where it went.
 *
 * You do not follow it -- which is the right default for a key you might press
 * twice, and useless without a word about where the thing landed. The name is read
 * *before* the move and the index *after*: emptying this tab removes it and shifts
 * everything after, so the number you would have printed first is not the number
 * anybody will see. */
static bool push_pane_a_tab(app_t *a, bool forward) {
  if (a->ntabs < 2) {
    app_toast(a, "only one tab to move it to");
    return true;
  }
  size_t to = (a->cur + (forward ? 1 : a->ntabs - 1)) % a->ntabs;
  uint32_t tid = a->tabs[to].id;
  char name[64];
  snprintf(name, sizeof name, "%s", a->tabs[to].name);

  if (!app_move_pane_to_tab(a, 0, tid, false)) {
    app_toast(a, "cannot move it there");
    return true;
  }
  size_t at = 0;
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].id == tid) at = i;
  char msg[128];
  if (name[0])
    snprintf(msg, sizeof msg, "moved to tab %zu (%s)", at + 1, name);
  else
    snprintf(msg, sizeof msg, "moved to tab %zu", at + 1);
  app_toast(a, msg);
  return true;
}

/* A pane dropped on the tab strip. The keys say this with a word and a drag says it
 * with a gesture; both end in the same two calls.
 *
 * The toast matters more here than for a key: the pane leaves the tab you are
 * looking at and lands somewhere you are not, so without a word the screen simply
 * shows one fewer pane. */
static void drop_pane_on_strip(app_t *a) {
  uint32_t src = a->drag.src;
  if (a->drag.new_tab_target) {
    app_toast(a, app_move_pane_to_new_tab(a, src, "")
                     ? "into a tab of its own"
                     : "it is the only pane in this tab");
    return;
  }
  uint32_t tid = a->drag.tab_target;
  char name[64] = {0};
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].id == tid)
      snprintf(name, sizeof name, "%s", a->tabs[i].name);

  if (!app_move_pane_to_tab(a, src, tid, false)) {
    /* The one refusal a drag can reach: the tab it is already in. */
    app_toast(a, "it is already in that tab");
    return;
  }
  size_t at = 0;
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].id == tid) at = i;
  char msg[128];
  if (name[0])
    snprintf(msg, sizeof msg, "moved to tab %zu (%s)", at + 1, name);
  else
    snprintf(msg, sizeof msg, "moved to tab %zu", at + 1);
  app_toast(a, msg);
}

static bool run_action(app_t *a, action_t act);

/* Do what a row says it does. One entry point for the keyboard and the mouse,
 * so a picker cannot choose one way with Enter and another with a click. */
static void picker_accept(app_t *a, const char *action) {
  if (strncmp(action, "find:", 5) == 0)
    app_focus_pane(a, (uint32_t)strtoul(action + 5, NULL, 10));
  else if (strncmp(action, "run:", 4) == 0)
    run_action(a, (action_t)strtol(action + 4, NULL, 10));
  else if (strncmp(action, "open:", 5) == 0) {
    /* By index into the snapshot the picker is showing, so the row that was
     * clicked is the project that opens even if the disk has changed since --
     * a scan between the paint and the click would otherwise open its
     * neighbour. */
    size_t i = (size_t)strtoul(action + 5, NULL, 10);
    if (i >= a->nprojects) return;
    app_workspace_open_t w;
    char err[256] = {0};
    if (app_workspace_open(a, a->projects[i].path, false, &w, err, sizeof err))
      app_toast(a, w.created ? "opened" : "already open");
    else
      app_toast(a, err[0] ? err : "cannot open that");
  }
}

/* Move the selection by `d`, wrapping.
 *
 * Wrapping because the list is short and the selection starts at the top: one
 * press of Up to reach the last entry is worth more here than the protection
 * against overshooting that a long list wants. */
static void picker_move(app_t *a, int d) {
  size_t n = picker_count(a);
  if (!n) {
    a->sel = 0;
    return;
  }
  int64_t sel = (int64_t)a->sel + d;
  int64_t last = (int64_t)n - 1;
  if (sel < 0) sel = d == -1 ? last : 0; /* a page up stops at the top */
  if (sel > last) sel = d == 1 ? 0 : last;
  a->sel = (size_t)sel;
}

/* Returns true when the picker consumed the event.
 *
 * It consumes *everything* while it is open, including keys it does nothing
 * with: it is a text field, and a text field that lets an unrecognised key
 * fall through to the pane behind it types into that pane. */
static bool picker_key(app_t *a, const input_event_t *ev) {
  if (!a->picker) return false;
  if (ev->kind != EV_KEY || ev->action == KEY_RELEASE) return true;

  bool ctrl = (ev->mods & MOD_CTRL) != 0;
  bool shift = (ev->mods & MOD_SHIFT) != 0;

  /* The emacs pair every picker in this shape answers to, and which the hands
   * of anyone who has used one reach for before the arrows. */
  if (ctrl && (ev->key == GHOSTTY_KEY_N || ev->unshifted == 'n')) {
    picker_move(a, 1);
    return true;
  }
  if (ctrl && (ev->key == GHOSTTY_KEY_P || ev->unshifted == 'p')) {
    picker_move(a, -1);
    return true;
  }
  /* Clear the query rather than the whole picker: the shell's own C-u, and
   * the alternative is backspacing a long wrong guess out one key at a time. */
  if (ctrl && (ev->key == GHOSTTY_KEY_U || ev->unshifted == 'u')) {
    a->query[0] = 0;
    a->sel = 0;
    return true;
  }

  switch (ev->key) {
  case GHOSTTY_KEY_ESCAPE: a->picker = PICK_NONE; return true;
  case GHOSTTY_KEY_ENTER: {
    pick_row_t rows[128];
    size_t n = picker_rows(a, rows, 128);
    /* The row's own action, the one a click on it would run: a picker that
       * chose by keyboard down one path and by mouse down another would be
       * two pickers wearing one box. */
    char action[48] = "";
    if (a->sel < n) snprintf(action, sizeof action, "%s", rows[a->sel].action);
    a->picker = PICK_NONE;
    if (*action) picker_accept(a, action);
    return true;
  }
  case GHOSTTY_KEY_TAB: picker_move(a, shift ? -1 : 1); return true;
  case GHOSTTY_KEY_ARROW_DOWN: picker_move(a, 1); return true;
  case GHOSTTY_KEY_ARROW_UP: picker_move(a, -1); return true;
  case GHOSTTY_KEY_PAGE_DOWN: picker_move(a, FINDER_ROWS); return true;
  case GHOSTTY_KEY_PAGE_UP: picker_move(a, -FINDER_ROWS); return true;
  case GHOSTTY_KEY_HOME: a->sel = 0; return true;
  case GHOSTTY_KEY_END: {
    size_t n = picker_count(a);
    a->sel = n ? n - 1 : 0;
    return true;
  }
  case GHOSTTY_KEY_BACKSPACE: {
    /* A character, not a byte: the query can hold whatever a pane's title
       * did, and half a UTF-8 sequence is not a search. (The rename editor
       * learned this first; this one was still eating bytes.) */
    size_t l = strlen(a->query);
    while (l && ((unsigned char)a->query[l - 1] & 0xC0) == 0x80) l--;
    if (l) l--;
    a->query[l] = 0;
    a->sel = 0;
    return true;
  }
  default: break;
  }
  if (!ctrl && ev->text_len && (unsigned char)ev->text[0] >= 0x20) {
    size_t l = strlen(a->query);
    if (l + ev->text_len < sizeof a->query) {
      memcpy(a->query + l, ev->text, ev->text_len);
      a->query[l + ev->text_len] = 0;
      a->sel = 0;
    }
  }
  return true;
}

/* ---- renaming a pane or a tab ------------------------------------------- */

/* The editor lives in the label itself — a pane's title cell (draw_frame) or a
 * tab's cell in the strip (draw_tab_strip) — so the name is typed where it is
 * going to live. There is no dialog to place, nothing to dismiss, and what is
 * being renamed cannot be in doubt. */
static void rename_begin(app_t *a, uint32_t id) {
  node_t *n = pane_by_id(a, id);
  if (!n) return;
  a->renaming = RENAME_PANE;
  a->rename_id = id;
  /* Seed with what is on screen: a rename is usually an edit, not a retype. */
  const char *shown = pane_title(n->pane);
  snprintf(a->rename_buf, sizeof a->rename_buf, "%s", shown ? shown : "");
  a->name_click_ms = 0;
  a->name_click_id = 0;
  app_focus_pane(a, id);
}

/* The same editor, on the pane's other label. A purpose is what tooling finds a
 * pane by (D8), and until now it could only be declared by a layout or set over
 * the socket -- so every pane anybody arranged by hand had none, and a layout
 * dumped from one was a shape with no tags in it. Typed here it counts as
 * *declared*, because D8's `declared` means "from a layout or an operator" and
 * a person at the keyboard is the operator. */
static void purpose_begin(app_t *a, uint32_t id) {
  node_t *n = pane_by_id(a, id);
  if (!n) return;
  a->renaming = RENAME_PURPOSE;
  a->rename_id = id;
  /* Seeded, unlike a tab's name: here the purpose *is* the thing being edited,
   * and the usual edit is adding `:2` to the end of one. */
  snprintf(a->rename_buf, sizeof a->rename_buf, "%s", n->purpose);
  a->name_click_ms = 0;
  a->name_click_id = 0;
  app_focus_pane(a, id);
}

static void rename_tab_begin(app_t *a, uint32_t id) {
  tab_t *t = tab_by_id(a, id);
  if (!t) return;
  a->renaming = RENAME_TAB;
  a->rename_id = id;
  /* Seeded with the name only. A pane seeds with its title because a title is
   * always something; a tab shows its *number* until it is named, and a number
   * is not a name you are editing — offering "3" to backspace over would be
   * offering to edit the wrong thing. A purpose is not seeded either: it comes
   * from a layout and outranks what is typed here. */
  snprintf(a->rename_buf, sizeof a->rename_buf, "%s", t->name);
  a->name_click_ms = 0;
  a->name_click_id = 0;
  app_select_tab_id(a, id);
}

static void rename_end(app_t *a, bool keep) {
  if (!a->renaming) return;
  if (keep) {
    bool done = false;
    const char *what = "name";
    if (a->renaming == RENAME_PURPOSE) {
      what = "purpose";
      done = app_set_pane_purpose(a, a->rename_id, a->rename_buf, true);
    } else if (a->renaming == RENAME_PANE) {
      done = app_set_pane_name(a, a->rename_id, a->rename_buf);
    } else {
      done = app_set_tab_name(a, a->rename_id, a->rename_buf);
    }
    if (done) {
      char said[40];
      snprintf(said, sizeof said, "%s %s", what,
               a->rename_buf[0] ? "set" : "cleared");
      /* "renamed" for a name, because that is the word for it; "purpose set"
       * for the other, because "renamed" would be a lie about which label
       * moved. */
      app_toast(a, a->renaming == RENAME_PURPOSE
                       ? said
                       : (a->rename_buf[0] ? "renamed" : "name cleared"));
    }
  }
  a->renaming = RENAME_NONE;
  a->rename_id = 0;
  a->rename_buf[0] = 0;
}

/* Returns true when the rename editor consumed the event. */
static bool rename_key(app_t *a, const input_event_t *ev) {
  if (!a->renaming) return false;
  if (ev->kind != EV_KEY || ev->action == KEY_RELEASE) return true;

  switch (ev->key) {
  case GHOSTTY_KEY_ESCAPE: rename_end(a, false); return true;
  case GHOSTTY_KEY_ENTER: rename_end(a, true); return true;
  case GHOSTTY_KEY_BACKSPACE: {
    /* A character, not a byte: a title is whatever the program could set,
       * and half a UTF-8 sequence is not a name. */
    size_t l = strlen(a->rename_buf);
    while (l && ((unsigned char)a->rename_buf[l - 1] & 0xC0) == 0x80) l--;
    if (l) l--;
    a->rename_buf[l] = 0;
    return true;
  }
  default: break;
  }
  if (ev->text_len && (unsigned char)ev->text[0] >= 0x20) {
    size_t l = strlen(a->rename_buf);
    if (l + ev->text_len < sizeof a->rename_buf) {
      memcpy(a->rename_buf + l, ev->text, ev->text_len);
      a->rename_buf[l + ev->text_len] = 0;
    }
  }
  return true;
}

void app_compose(app_t *a, screen_t *s) {
  screen_clear(s); /* every frame starts blank: no ghosts in the gap ring */
  hit_reset(&s->hits);
  s->cursor_visible = false;
  a->painted = s;
  /* Re-derived by the shader passes below, every frame, from what they ran. */
  a->animating = false;
  if (!a->ntabs || !cur(a)->root) return;

  /* Looking at a pane is the acknowledgement, and it happens before anything
   * is drawn rather than while the pane itself is: the tab strip is painted
   * first and would otherwise spend a frame reporting a bell that had just
   * been answered. Done here so that every route to "this pane is focused"
   * clears it, not only the ones that thought to. */
  if (cur(a)->focus) pane_clear_bell(cur(a)->focus->pane);

  layout(a);
  find_corners(a);
  if (CFG.status_bar) draw_tab_strip(a, s);
  draw_node(a, s, cur(a)->root);
  draw_corners(a, s);
  draw_min_bar(a, s);
  draw_status_line(a, s);
  /* The modals, on top of everything: each is the only thing that can be
   * interacted with while it is up, so it owns the topmost hits. The scrim
   * goes down first, over everything already painted, and once for both —
   * dimming twice would make the second one darker for no reason. */
  if (a->picker || a->help) draw_scrim(a, s);
  if (a->picker) draw_picker(a, s);
  if (a->help) draw_help(a, s);
  draw_toasts(a, s); /* and above even that: it is transient */
}

/* ---- kitty graphics ------------------------------------------------------ */

struct gfx_ctx {
  app_t *a;
  node_t *leaf;
};

static void gfx_from_pane(pane_t *p, const pane_gfx_t *g, void *ud) {
  struct gfx_ctx *c = ud;
  node_t *leaf = c->leaf;

  /* Pane-local viewport cells become screen cells, and anything hanging over
   * the pane's edge is cropped by asking for fewer columns and rows — the
   * only clipping the protocol lets us do without knowing the client's cell
   * size in pixels. */
  if (g->col >= leaf->content.w || g->row >= leaf->content.h) return;
  uint16_t cols = g->cols, rows = g->rows;
  uint32_t sw = g->sw, sh = g->sh;
  /* Cropping means moving the source rectangle, and how much source a lost
   * cell is worth depends on whether the image is being scaled:
   *
   *   natural size  one source pixel is one screen pixel, so the crop is the
   *                 screen pixels the pane has left -- measured from where
   *                 the image starts, which is `x_off` into its first cell
   *                 and not at the cell's edge. Cropping by whole cells alone
   *                 let an offset image hang up to a cell over the border.
   *   scaled        the program asked for c=/r=, so a cell is worth
   *                 source/cells pixels and the crop is proportional. Taking
   *                 screen pixels off a scaled image would crop far too much
   *                 -- a 4px image across 40 cells loses its whole self. */
  if (g->col + cols > leaf->content.w) {
    uint16_t avail = (uint16_t)(leaf->content.w - g->col);
    if (g->req_cols) {
      sw = cols ? (uint32_t)((uint64_t)sw * avail / cols) : sw;
    } else {
      uint32_t px = (uint32_t)avail * g->cell_px_w;
      px = px > g->x_off ? px - g->x_off : 0;
      if (sw > px) sw = px;
    }
    cols = avail;
  }
  if (g->row + rows > leaf->content.h) {
    uint16_t avail = (uint16_t)(leaf->content.h - g->row);
    if (g->req_rows) {
      sh = rows ? (uint32_t)((uint64_t)sh * avail / rows) : sh;
    } else {
      uint32_t px = (uint32_t)avail * g->cell_px_h;
      px = px > g->y_off ? px - g->y_off : 0;
      if (sh > px) sh = px;
    }
    rows = avail;
  }
  if (!cols || !rows) return;

  gfx_place(
      c->a->gfx,
      &(gfx_req_t){
          .pane = leaf->id,
          .src_id = g->image_id,
          .gen = g->generation,
          .place_id = g->place_id,
          .col = (uint16_t)(leaf->content.x + g->col),
          .row = (uint16_t)(leaf->content.y + g->row),
          .cols = cols,
          .rows = rows,
          /* Carried through untouched: the pane's own clipping already dealt with
       * it, and the offset is relative to the first cell either way. */
          .x_off = g->x_off,
          .y_off = g->y_off,
          /* Clipped the same way the cell counts were, and zero when the program
       * never asked to scale. */
          .scale_cols = g->req_cols ? cols : 0,
          .scale_rows = g->req_rows ? rows : 0,
          .sx = g->sx,
          .sy = g->sy,
          .sw = sw,
          .sh = sh,
          .px_w = g->src_w,
          .px_h = g->src_h,
          .format = g->format,
          .compression = g->compression,
          .data = g->data,
          .data_len = g->data_len,
      });
}

static void gfx_leaf_cb(node_t *n, void *ud) {
  app_t *a = ud;
  if (n->hidden) return; /* a collapsed pane draws nothing, images included */
  struct gfx_ctx ctx = {a, n};
  pane_graphics(n->pane, gfx_from_pane, &ctx);
}

/* Walk the visible tab's panes and produce the bytes the client's terminal
 * needs this frame. Borrowed until the next call. */
const char *app_graphics(app_t *a, size_t *len) {
  gfx_begin(a->gfx);
  if (a->ntabs && cur(a)->root) walk(cur(a)->root, gfx_leaf_cb, a);
  return gfx_flush(a->gfx, len);
}

void app_graphics_reset(app_t *a) { gfx_reset(a->gfx); }

char *app_graphics_json(app_t *a) {
  size_t len = 0;
  app_graphics(a, &len); /* refresh the model; the bytes are the caller's job */
  const gfx_place_t *places = NULL;
  size_t n = gfx_placements(a->gfx, &places);
  json_t j;
  json_init(&j);
  json_arr_open(&j, NULL);
  for (size_t i = 0; i < n; i++) {
    json_obj_open(&j, NULL);
    json_int(&j, "image", places[i].out_id);
    json_int(&j, "placement", places[i].place_id);
    json_int(&j, "x", places[i].col);
    json_int(&j, "y", places[i].row);
    json_int(&j, "cols", places[i].cols);
    json_int(&j, "rows", places[i].rows);
    /* Reported so a test can see sub-cell motion, which is otherwise only
     * visible as "the picture moves smoothly" on somebody's screen. */
    json_int(&j, "x_off", places[i].x_off);
    json_int(&j, "y_off", places[i].y_off);
    json_obj_close(&j);
  }
  json_arr_close(&j);
  return j.buf;
}

/* ---- input -------------------------------------------------------------- */

static node_t *by_id(app_t *a, uint32_t id) {
  struct byid b = {id, NULL};
  walk(cur(a)->root, byid_cb, &b);
  return b.found;
}

static node_t *split_by_id(app_t *a, uint32_t id) {
  /* splits are not leaves, so walk the trees rather than the leaf walker */
  for (size_t t = 0; t < a->ntabs; t++) {
    node_t *stack[64];
    size_t n = 0;
    if (a->tabs[t].root) stack[n++] = a->tabs[t].root;
    while (n) {
      node_t *cur_ = stack[--n];
      if (cur_->kind == NODE_SPLIT) {
        if (cur_->id == id) return cur_;
        for (size_t i = 0; i < cur_->nkids && n < 64; i++)
          stack[n++] = cur_->kids[i];
      }
    }
  }
  return NULL;
}

/* Drag a boundary by `cells`, in the split's own direction. */
static void drag_edge(app_t *a, node_t *sp, size_t i, int cells) {
  if (!sp || i + 1 >= sp->nkids || cells == 0) return;
  uint16_t span = sp->dir == SPLIT_COLS ? sp->rect.w : sp->rect.h;
  if (!span) return;

  long total = 0;
  for (size_t k = 0; k < sp->nkids; k++) total += sp->kids[k]->weight;
  int amount = (int)((long)labs(cells) * total / (span ? span : 1));
  if (amount <= 0) amount = 1;

  if (cells > 0)
    transfer_weight(sp->kids[i + 1], sp->kids[i], amount);
  else
    transfer_weight(sp->kids[i], sp->kids[i + 1], amount);
  layout(a);
}

/* Focus follows the mouse, but never at the cost of what you were doing.
 *
 * Hovering may not: steal focus mid-chord (the prefix is held), reach past an
 * open finder, interrupt a drag, or expand a collapsed pane just because the
 * pointer crossed its header — which is why only `pane:` and `title:` targets
 * count, and the frame rect's `focus:` does not. */
static bool hover_focus_allowed(const app_t *a) {
  return CFG.focus_follows_mouse && !a->prefix && !a->picker && !a->renaming &&
         a->drag.kind == DRAG_NONE;
}

static void do_action(app_t *a, const char *action, const input_event_t *ev) {
  bool on_name = strncmp(action, "panetitle:", 10) == 0;
  if (on_name || strncmp(action, "title:", 6) == 0) {
    uint32_t id = (uint32_t)strtoul(action + (on_name ? 10 : 6), NULL, 10);
    if (ev->maction == MOUSE_MOTION && hover_focus_allowed(a)) {
      app_focus_pane(a, id);
      return;
    }
    if (ev->maction == MOUSE_PRESS) {
      /* Two clicks on the name, close together, rename the pane. This is why
       * the name is not an edge: the first click must not have split, or the
       * second would be renaming a pane that had already moved. */
      if (on_name) {
        int64_t now = now_ms_();
        if (a->name_click_id == id && a->name_click_kind == RENAME_PANE &&
            now - a->name_click_ms <= (int64_t)CFG.double_click_ms) {
          rename_begin(a, id);
          return; /* a rename, not a grab: start no drag */
        }
        a->name_click_ms = now;
        a->name_click_id = id;
        a->name_click_kind = RENAME_PANE;
      }
      /* The top border is both a drag handle and an edge, and whether this is a
       * move or a split-upward is decided by whether the pointer moves. Which
       * side it takes is decided by *where* on the row it was pressed: only the
       * handle -- `title:<id>:t`, the middle -- carries one. The row itself and
       * the title text take none, so a click that never moved does nothing
       * there rather than splitting a layout you were only reaching across. */
      a->drag.kind = DRAG_TITLE;
      a->drag.src = id;
      a->drag.target = id;
      a->drag.x = ev->mx;
      a->drag.y = ev->my;
      a->drag.moved = false;
      const char *sep = on_name ? NULL : strchr(action + 6, ':');
      a->drag.side = sep && sep[1] ? sep[1] : 0;
      app_focus_pane(a, id);
    }
    return;
  }
  /* The rim of an edge: hover arms the guide, and a press does nothing at all.
   * Deliberately inert and deliberately explicit -- this is the whole point of
   * the handle, and a silent fall-through to whatever comes next would be a
   * thin place for it to live. Starting no drag also means the release path
   * finds nothing to split, which is what stops the accidents. */
  if (strncmp(action, "brim:", 5) == 0) return;
  if (strncmp(action, "border:", 7) == 0) {
    uint32_t id = (uint32_t)strtoul(action + 7, NULL, 10);
    const char *colon = strchr(action + 7, ':');
    char side = colon && colon[1] ? colon[1] : 'r';
    if (ev->maction == MOUSE_PRESS) {
      a->drag.kind = DRAG_BORDER;
      a->drag.src = id;
      a->drag.side = side;
      a->drag.moved = false;
      a->drag.x = ev->mx;
      a->drag.y = ev->my;
    }
    return;
  }
  if (strncmp(action, "corner:", 7) == 0) {
    if (ev->maction != MOUSE_PRESS) return;
    size_t ci = strtoul(action + 7, NULL, 10);
    if (ci >= a->ncorners) return;
    corner_t *c = &a->corners[ci];
    a->drag.kind = DRAG_EDGE;
    a->drag.c_nh = c->nh;
    for (size_t k = 0; k < c->nh; k++) {
      a->drag.c_h[k] = c->h_id[k];
      a->drag.c_hedge[k] = c->h_edge[k];
    }
    a->drag.c_nv = c->nv;
    for (size_t k = 0; k < c->nv; k++) {
      a->drag.c_v[k] = c->v_id[k];
      a->drag.c_vedge[k] = c->v_edge[k];
    }
    a->drag.moved = false;
    a->drag.x = ev->mx;
    a->drag.y = ev->my;
    return;
  }
  if (strncmp(action, "edge:", 5) == 0) {
    if (ev->maction != MOUSE_PRESS) return;
    a->drag.kind = DRAG_EDGE;
    a->drag.c_nv = 0;
    a->drag.c_nh = 0;
    a->drag.src = (uint32_t)strtoul(action + 5, NULL, 10);
    const char *colon = strchr(action + 5, ':');
    a->drag.edge = colon ? strtoul(colon + 1, NULL, 10) : 0;
    a->drag.x = ev->mx;
    a->drag.y = ev->my;
    return;
  }
  if (strncmp(action, "btn:", 4) == 0) {
    if (ev->maction != MOUSE_PRESS) return;
    uint32_t id = (uint32_t)strtoul(action + 4, NULL, 10);
    const char *bid = strchr(action + 4, ':');
    node_t *n = pane_by_id(a, id);
    if (n && bid) {
      app_focus_pane(a, id);
      pane_click_button(n->pane, bid + 1);
    }
    return;
  }
  if (strcmp(action, "newtab") == 0) {
    if (ev->maction == MOUSE_PRESS) app_new_tab(a, "");
    return;
  }
  if (strncmp(action, "find:", 5) == 0 || strncmp(action, "run:", 4) == 0 ||
      strncmp(action, "open:", 5) == 0) {
    if (ev->maction != MOUSE_PRESS) return;
    /* Closed first, so an action that opens something else -- the cheatsheet,
     * or the palette again -- is not shut by the picker it came from. */
    a->picker = PICK_NONE;
    picker_accept(a, action);
    return;
  }
  if (strcmp(action, "closefind") == 0 || strcmp(action, "closepalette") == 0 ||
      strcmp(action, "closeprojects") == 0) {
    if (ev->maction == MOUSE_PRESS) a->picker = PICK_NONE;
    return;
  }
  if (strncmp(action, "tab:", 4) == 0) {
    if (ev->maction != MOUSE_PRESS) return;
    uint32_t id = (uint32_t)strtoul(action + 4, NULL, 10);
    /* Two clicks on a tab, close together, rename it — the same gesture as a
     * pane's title, on the other thing that has a name. The first click still
     * switches to it, which is what you want either way: you rename the tab
     * you are now in. */
    int64_t now = now_ms_();
    if (a->name_click_id == id && a->name_click_kind == RENAME_TAB &&
        now - a->name_click_ms <= (int64_t)CFG.double_click_ms) {
      rename_tab_begin(a, id);
      return;
    }
    a->name_click_ms = now;
    a->name_click_id = id;
    a->name_click_kind = RENAME_TAB;
    app_select_tab_id(a, id);
    /* And it is now held: moving the pointer onto another tab reorders the
     * strip. Armed on every press that is not a rename, exactly like a pane's
     * title — a press that never moves stays the click that switched tabs,
     * because the reorder only happens on motion. */
    a->drag.kind = DRAG_TAB;
    a->drag.src = id;
    a->drag.target = id;
    a->drag.moved = false;
    a->drag.side = 0; /* shared with the border drag, which owns that field */
    a->drag.x = ev->mx;
    a->drag.y = ev->my;
    return;
  }
  /* Chrome activates on press. Forwarding to a pane does not: an app wants the
   * release and the motion too. Without this, one click of the split button
   * splits twice — press and release both landing on the same target. */
  bool to_pane = strncmp(action, "pane:", 5) == 0;
  if (!to_pane && ev->maction != MOUSE_PRESS) return;

  uint32_t id = 0;
  const char *colon = strchr(action, ':');
  if (colon) id = (uint32_t)strtoul(colon + 1, NULL, 10);
  node_t *n = by_id(a, id);
  if (!n) return;

  if (strncmp(action, "split:", 6) == 0) {
    cur(a)->focus = n;
    split_focus(a, SPLIT_COLS);
  } else if (strncmp(action, "minimize:", 9) == 0) {
    if (!app_minimize(a, n->id)) app_toast(a, "nothing else to show");
  } else if (strncmp(action, "zoom:", 5) == 0) {
    app_toggle_zoom(a, n->id);
  } else if (strncmp(action, "close:", 6) == 0) {
    close_leaf(a, n);
  } else if (strncmp(action, "rerun:", 6) == 0) {
    app_rerun_pane(a, n->id);
  } else if (strncmp(action, "scrollbottom:", 13) == 0) {
    if (ev->maction == MOUSE_PRESS) pane_scroll_edge(n->pane, false);
  } else if (strncmp(action, "pane:", 5) == 0) {
    /* A press always focuses; a hover only when it is allowed to. The event
     * is forwarded either way, so a pane that tracks the mouse still sees the
     * pointer cross it whether or not focus moved. */
    if (ev->maction != MOUSE_MOTION || hover_focus_allowed(a))
      cur(a)->focus = n;
    /* translate to pane-local coordinates before forwarding */
    input_event_t local = *ev;
    local.mx = (uint16_t)(ev->mx - n->content.x);
    local.my = (uint16_t)(ev->my - n->content.y);

    /* The wheel belongs to whoever can use it:
     *   - a program tracking the mouse gets the wheel events;
     *   - a full-screen program without mouse tracking (vim, less) has no
     *     scrollback of ours to show, so the wheel becomes arrow keys, which
     *     is what it means to that program;
     *   - anything else scrolls our scrollback. */
    /* Left press on a pane that is not tracking the mouse starts a selection.
     * A program that wants the mouse keeps it; shift is the usual escape
     * hatch for selecting over one of those, and it is what people already
     * press out of habit. */
    bool select_press = ev->maction == MOUSE_PRESS && ev->button == MBTN_LEFT &&
                        (!pane_wants_mouse(n->pane) || (ev->mods & MOD_SHIFT));
    if (select_press) {
      int64_t now = now_ms_();
      bool again = a->cell_click_id == n->id && a->cell_click_x == local.mx &&
                   a->cell_click_y == local.my &&
                   now - a->cell_click_ms <= (int64_t)CFG.double_click_ms;
      a->cell_click_id = n->id;
      a->cell_click_x = local.mx;
      a->cell_click_y = local.my;
      a->cell_click_ms = now;

      /* Start first, then widen to the word: the anchor start() leaves behind is
       * what a drag out of the double-click extends from, and it means the
       * release path below copies this selection like any other. The word *is*
       * the copy, in one place, rather than a second copy site here. */
      pane_select_start(n->pane, local.mx, local.my);
      if (again)
        pane_select_word(n->pane, local.mx, local.my, CFG.word_separators);
      a->drag.kind = DRAG_SELECT;
      a->drag.src = n->id;
      return;
    }

    /* Middle click pastes the last selection, the way a primary selection
     * behaves everywhere else. */
    if (ev->maction == MOUSE_PRESS && ev->button == MBTN_MIDDLE &&
        !pane_wants_mouse(n->pane) && a->clipboard) {
      pane_send_paste(n->pane, a->clipboard, strlen(a->clipboard));
      return;
    }

    bool wheel = ev->maction == MOUSE_PRESS &&
                 (ev->button == MBTN_FOUR || ev->button == MBTN_FIVE);
    if (wheel && !pane_wants_mouse(n->pane)) {
      int lines = CFG.scroll_lines;
      bool up = ev->button == MBTN_FOUR;
      if (pane_alt_screen(n->pane)) {
        input_event_t key = {.kind = EV_KEY, .action = KEY_PRESS};
        key.key = up ? GHOSTTY_KEY_ARROW_UP : GHOSTTY_KEY_ARROW_DOWN;
        for (int i = 0; i < lines; i++) pane_send_key(n->pane, &key);
      } else {
        pane_scroll(n->pane, up ? -lines : lines);
      }
      return;
    }
    pane_send_mouse(n->pane, &local);
  } else if (strncmp(action, "focus:", 6) == 0) {
    cur(a)->focus = n;
  }
}

static bool prefix_command(app_t *a, const input_event_t *ev) {
  return run_action(a, config_lookup(&CFG, ev->key, ev->mods));
}

/* What an action does, given the action rather than the key that asked for it
 * -- because the command palette asks for actions by name, and a palette that
 * had to synthesise a keystroke to run one would be inventing input. */
static bool run_action(app_t *a, action_t act) {
  if (act >= ACT_SELECT_TAB_1) {
    app_select_tab(a, (size_t)(act - ACT_SELECT_TAB_1));
    return true;
  }
  switch (act) {
  case ACT_SPLIT: split_focus_auto(a); return true;
  case ACT_SPLIT_COLS: split_focus_ui(a, SPLIT_COLS); return true;
  case ACT_SPLIT_ROWS: split_focus_ui(a, SPLIT_ROWS); return true;
  case ACT_ZOOM: app_toggle_zoom(a, 0); return true;
  case ACT_MINIMIZE:
    if (!app_minimize(a, 0)) app_toast(a, "nothing else to show");
    return true;
  case ACT_CLOSE_PANE:
    if (cur(a)->focus) close_leaf(a, cur(a)->focus);
    return true;
  case ACT_SET_PURPOSE:
    /* Nothing to tag in an empty tab, and a purpose belongs to a pane rather
       * than to the space one would occupy. */
    if (cur(a)->focus) purpose_begin(a, cur(a)->focus->id);
    return true;
  case ACT_RERUN:
    /* The keyboard's half of a dead pane's [re-run] button. Refused on a
       * live pane rather than restarting it: "run it again" would mean
       * killing something that is still working, which is not what anybody
       * pressing it is asking for. */
    if (cur(a)->focus && pane_alive(cur(a)->focus->pane) &&
        !pane_suspended(cur(a)->focus->pane))
      app_toast(a, "still running");
    else
      app_rerun_pane(a, 0);
    return true;
  case ACT_FOCUS_LEFT: focus_dir(a, -1, 0); return true;
  case ACT_FOCUS_RIGHT: focus_dir(a, 1, 0); return true;
  case ACT_FOCUS_UP: focus_dir(a, 0, -1); return true;
  case ACT_FOCUS_DOWN: focus_dir(a, 0, 1); return true;
  case ACT_FOCUS_NEXT: focus_next(a); return true;
  case ACT_NEW_TAB: app_new_tab(a, ""); return true;
  case ACT_CLOSE_TAB: {
    /* The last tab is refused rather than obeyed. `app_close_tab` ends the
       * session when nothing is left, which is right for a *request* -- a script
       * asking to close the only tab means it -- and a trap for a key: one that
       * closes a tab four times and ends your session the fifth is a key you
       * cannot press without counting first. `quit` is how you mean that, and it
       * is one letter away. */
    if (a->ntabs < 2) {
      app_toast(a, "last tab: quit the session instead");
      return true;
    }
    /* Said out loud with the count, because everything in it goes with it and
       * the panes it kills are the ones you were not looking at. */
    size_t n = count_leaves(cur(a)->root);
    char said[64];
    snprintf(said, sizeof said, "closed tab \u00b7 %zu pane%s", n,
             n == 1 ? "" : "s");
    if (app_close_tab(a, cur(a)->id)) app_toast(a, said);
    return true;
  }
  case ACT_NEXT_TAB: app_cycle_tab(a, 1); return true;
  case ACT_PREV_TAB: app_cycle_tab(a, -1); return true;
  case ACT_RESIZE_LEFT: resize_focus(a, -1, 0); return true;
  case ACT_RESIZE_RIGHT: resize_focus(a, 1, 0); return true;
  case ACT_RESIZE_UP: resize_focus(a, 0, -1); return true;
  case ACT_RESIZE_DOWN: resize_focus(a, 0, 1); return true;
  case ACT_CLEAR_SHADERS:
    /* Says which of the two happened. "Nothing to undo" and "undone" look
       * identical on a pane that was never painted, and a key that might have
       * done nothing is a key you press again. */
    app_toast(a, app_clear_pane_shaders(a, 0) ? "shaders cleared"
                                              : "no shaders on this pane");
    return true;
  case ACT_PANE_TO_NEXT_TAB:
  case ACT_PANE_TO_PREV_TAB:
    return push_pane_a_tab(a, act == ACT_PANE_TO_NEXT_TAB);
  case ACT_PANE_TO_NEW_TAB: {
    uint32_t made = app_move_pane_to_new_tab(a, 0, "");
    app_toast(a, made ? "into a tab of its own"
                      : "it is the only pane in this tab");
    return true;
  }
  case ACT_EQUALIZE:
    if (!app_equalize_splits(a)) app_toast(a, "nothing to even out");
    return true;
  case ACT_ROTATE_LAYOUT: rotate_layout_ui(a); return true;
  case ACT_SCROLL_UP:
    pane_scroll(cur(a)->focus->pane, -CFG.scroll_lines);
    return true;
  case ACT_SCROLL_DOWN:
    pane_scroll(cur(a)->focus->pane, CFG.scroll_lines);
    return true;
  case ACT_SCROLL_PAGE_UP:
    pane_scroll(cur(a)->focus->pane, -(int)cur(a)->focus->content.h);
    return true;
  case ACT_SCROLL_PAGE_DOWN:
    pane_scroll(cur(a)->focus->pane, (int)cur(a)->focus->content.h);
    return true;
  case ACT_SCROLL_TOP: pane_scroll_edge(cur(a)->focus->pane, true); return true;
  case ACT_SCROLL_BOTTOM:
    pane_scroll_edge(cur(a)->focus->pane, false);
    return true;
  case ACT_FINDER:
  case ACT_PALETTE:
    a->picker = act == ACT_PALETTE ? PICK_PALETTE : PICK_FINDER;
    a->query[0] = 0;
    a->sel = 0;
    return true;
  case ACT_WORKSPACES: {
    /* Scanned here, once, and kept only while the picker is up: draw_picker
       * asks for its rows every frame. Opening is also exactly when the list
       * has to be right, which is why nothing older than this keystroke is
       * ever shown. */
    if (!app_project_roots_set()) {
      app_toast(a, "no project roots: set project_roots in your config");
      return true;
    }
    free(a->projects);
    a->projects = calloc(PROJECTS_MAX, sizeof *a->projects);
    a->nprojects = a->projects ? app_projects(a->projects, PROJECTS_MAX) : 0;
    if (!a->nprojects) {
      app_toast(a, "no projects under your project_roots");
      return true;
    }
    a->picker = PICK_WORKSPACES;
    a->query[0] = 0;
    a->sel = 0;
    return true;
  }
  case ACT_SAVE_WORKSPACE: {
    app_workspace_save_t w;
    char err[256] = {0};
    /* `commands` rather than as-is: the pane running this morning's dev server
       * should be in the file asleep, not started on every open. */
    if (!app_workspace_save(a, 0, NULL, DUMP_SUSPEND_COMMANDS, true, &w, err,
                            sizeof err)) {
      app_toast(a, err[0] ? err : "cannot save that");
      return true;
    }
    char said[96];
    snprintf(said, sizeof said, "%s %s \u00b7 %zu pane%s, %zu suspended",
             w.replaced ? "replaced" : "wrote", PROJECT_LAYOUT_FILE, w.panes,
             w.panes == 1 ? "" : "s", w.suspended);
    app_toast(a, said);
    return true;
  }
  case ACT_EDIT_CONFIG: app_edit_config(a); return true;
  case ACT_HELP: a->help = !a->help; return true;
  case ACT_DETACH: a->detach = true; return true;
  case ACT_QUIT: a->quit = true; return true;
  default: return false;
  }
}

void app_event(app_t *a, const input_event_t *ev) {
  if (!a->ntabs || !cur(a)->focus) return;

  /* A rename whose subject went away must not keep the keyboard. */
  if (a->renaming == RENAME_PANE && !pane_by_id(a, a->rename_id))
    rename_end(a, false);
  if (a->renaming == RENAME_TAB && !tab_by_id(a, a->rename_id))
    rename_end(a, false);

  /* The rename editor and the picker each own the keyboard while open; the
   * mouse still routes through the hit list, whose topmost entries are the
   * picker's own rows. */
  if (a->renaming && ev->kind == EV_KEY) {
    rename_key(a, ev);
    return;
  }
  if (a->picker && ev->kind == EV_KEY) {
    picker_key(a, ev);
    return;
  }
  /* A cheatsheet is read, not driven: any key puts it away, and that key does
   * nothing else. Swallowing it is the point -- dismissing a modal should not
   * also run the thing you happened to press. */
  if (a->help && ev->kind == EV_KEY) {
    if (ev->action != KEY_RELEASE) a->help = false;
    return;
  }

  /* A release can go missing (the pointer leaves the terminal, the client
   * detaches mid-drag). Any keystroke ends a drag, so the mouse can never be
   * left wedged in a state the user cannot see. */
  if (ev->kind == EV_KEY && a->drag.kind != DRAG_NONE) {
    if (a->drag.kind == DRAG_SELECT) {
      node_t *n = pane_by_id(a, a->drag.src);
      if (n) pane_select_done(n->pane);
    }
    a->drag.kind = DRAG_NONE;
    a->drag.src = a->drag.target = 0;
  }

  if (ev->kind == EV_KEY && ev->action != KEY_RELEASE) {
    uint16_t mods = ev->mods & (MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_SUPER);
    bool is_prefix = ev->key == CFG.prefix_key && mods == CFG.prefix_mods;
    if (a->prefix) {
      a->prefix = false;
      /* prefix twice sends the prefix itself, whatever it is bound to */
      if (is_prefix ||
          config_lookup(&CFG, ev->key, mods) == ACT_LITERAL_PREFIX) {
        input_event_t literal = *ev;
        literal.key = CFG.prefix_key;
        literal.mods = CFG.prefix_mods;
        pane_send_key(cur(a)->focus->pane, &literal);
        return;
      }
      prefix_command(a, ev); /* unbound: swallowed */
      return;
    }
    if (is_prefix) {
      a->prefix = true;
      return;
    }

    /* Bindings that need no leader. Last, so the leader itself and every
     * overlay that owns the keyboard have already had their say -- a direct
     * binding must not fire while you are typing a pane's new name, and it
     * must never shadow the prefix.
     *
     * Everything below this line goes to the program in the pane, so a chord
     * bound here is a chord that program can no longer see. That is the deal,
     * it is opt-in, and it is the user's keyboard. */
    action_t direct = config_lookup_direct(&CFG, ev->key, mods);
    if (direct != ACT_NONE && direct != ACT_LITERAL_PREFIX) {
      prefix_command(a, ev);
      return;
    }
  }

  switch (ev->kind) {
  case EV_KEY:
    /* the first keystroke starts a suspended pane, and is not forwarded */
    if (pane_suspended(cur(a)->focus->pane)) {
      pane_start(cur(a)->focus->pane);
      break;
    }
    pane_send_key(cur(a)->focus->pane, ev);
    break;
  case EV_MOUSE: {
    /* Mouse routing is a hit-list lookup, never a re-derivation of geometry:
       * the list was filled by the pass that painted what the user clicked. */
    if (!a->painted) break;
    const char *action = hit_test(&a->painted->hits, ev->mx, ev->my);

    /* Clicking away keeps the name, the way leaving a field commits it.
       * Clicking the thing's own label again does not, so a stray second
       * double-click lands in the editor instead of closing it. */
    if (a->renaming && ev->maction == MOUSE_PRESS) {
      char own[48];
      snprintf(own, sizeof own,
               a->renaming == RENAME_PANE ? "panetitle:%u" : "tab:%u",
               a->rename_id);
      if (!action || strcmp(action, own) != 0) rename_end(a, true);
    }

    if (!a->ptr_valid || ev->mx != a->ptr_x || ev->my != a->ptr_y)
      a->ptr_still_since = now_ms_();
    a->ptr_x = ev->mx;
    a->ptr_y = ev->my;
    a->ptr_valid = true;

    if (a->drag.kind != DRAG_NONE) {
      if (ev->maction == MOUSE_MOTION) {
        a->drag.moved = true;
        if (a->drag.kind == DRAG_SELECT) {
          node_t *n = pane_by_id(a, a->drag.src);
          if (n)
            pane_select_extend(n->pane, (uint16_t)(ev->mx - n->content.x),
                               (uint16_t)(ev->my - n->content.y));
        } else if (a->drag.kind == DRAG_EDGE &&
                   (a->drag.c_nv || a->drag.c_nh)) {
          /* One axis to each: the row boundary takes the vertical movement
             * and the column boundaries the horizontal, so the corner follows
             * the pointer in both at once. The column boundaries above and
             * below get the same delta, which is what keeps them one line. */
          int dx = (int)ev->mx - (int)a->drag.x;
          int dy = (int)ev->my - (int)a->drag.y;
          for (size_t i = 0; i < a->drag.c_nh; i++)
            drag_edge(a, split_by_id(a, a->drag.c_h[i]), a->drag.c_hedge[i],
                      dy);
          for (size_t i = 0; i < a->drag.c_nv; i++)
            drag_edge(a, split_by_id(a, a->drag.c_v[i]), a->drag.c_vedge[i],
                      dx);
        } else if (a->drag.kind == DRAG_EDGE) {
          node_t *sp = split_by_id(a, a->drag.src);
          int cells = sp && sp->dir == SPLIT_COLS
                          ? (int)ev->mx - (int)a->drag.x
                          : (int)ev->my - (int)a->drag.y;
          drag_edge(a, sp, a->drag.edge, cells);
        } else if (a->drag.kind == DRAG_TAB) {
          /* Reordered as you drag, rather than dropped at the end: the
             * strip is the only thing that could show an insertion point, and
             * a strip that already shows the result needs no such invention.
             * Off the strip entirely, nothing moves — dragging away is not a
             * cancel, it is simply not a move. */
          size_t from = tab_index(a, a->drag.src);
          if (from != (size_t)-1 && action) {
            size_t to = (size_t)-1;
            if (strncmp(action, "tab:", 4) == 0)
              to = tab_index(a, (uint32_t)strtoul(action + 4, NULL, 10));
            else if (strcmp(action, "newtab") == 0)
              to = a->ntabs - 1; /* past the last tab means last */
            if (to != (size_t)-1) move_tab(a, from, to);
          }
        } else if (a->drag.kind == DRAG_TITLE && action &&
                   strncmp(action, "tab:", 4) == 0) {
          /* Over the strip: this pane is going to that tab. */
          a->drag.tab_target = (uint32_t)strtoul(action + 4, NULL, 10);
          a->drag.new_tab_target = false;
          a->drag.target = 0;
        } else if (a->drag.kind == DRAG_TITLE && action &&
                   strcmp(action, "newtab") == 0) {
          /* The button that makes a tab, used as somewhere to put one pane. */
          a->drag.new_tab_target = true;
          a->drag.tab_target = 0;
          a->drag.target = 0;
        } else if (action && strncmp(action, "title:", 6) == 0) {
          a->drag.target = (uint32_t)strtoul(action + 6, NULL, 10);
          a->drag.tab_target = 0;
          a->drag.new_tab_target = false;
        } else if (action && strncmp(action, "panetitle:", 10) == 0) {
          /* Dropping onto a pane's name is dropping onto that pane. */
          a->drag.target = (uint32_t)strtoul(action + 10, NULL, 10);
          a->drag.tab_target = 0;
          a->drag.new_tab_target = false;
        } else if (action && strncmp(action, "pane:", 5) == 0) {
          a->drag.target = (uint32_t)strtoul(action + 5, NULL, 10);
          a->drag.tab_target = 0;
          a->drag.new_tab_target = false;
        }
        a->drag.x = ev->mx;
        a->drag.y = ev->my;
      } else if (ev->maction == MOUSE_RELEASE) {
        /* A press that never moved is a click, and a click on an edge
           * splits toward it. */
        if (!a->drag.moved && a->drag.side &&
            (a->drag.kind == DRAG_BORDER || a->drag.kind == DRAG_TITLE)) {
          node_t *n = pane_by_id(a, a->drag.src);
          if (n) {
            char side = a->drag.side;
            bool before = side == 'l' || side == 't';
            split_dir_t dir = side_dir(side);
            /* The guide already declined to offer this, so the click that
               * the guide would have explained must decline too — otherwise
               * the border silently does something it just said it would not. */
            if (!split_fits(n, dir)) {
              app_toast(a, dir == SPLIT_COLS ? "no room to split across"
                                             : "no room to split down");
            } else {
              split_node(a, n, dir, before);
              app_toast(a, side == 'l'   ? "split left"
                           : side == 'r' ? "split right"
                           : side == 't' ? "split up"
                                         : "split down");
            }
          }
          a->drag.kind = DRAG_NONE;
          a->drag.src = a->drag.target = 0;
          break;
        }
        if (a->drag.kind == DRAG_SELECT) {
          /* Releasing copies, which is the whole point: no menu, no chord,
             * the selection *is* the copy. */
          node_t *n = pane_by_id(a, a->drag.src);
          if (n) {
            set_clipboard(a, pane_selection_text(n->pane));
            pane_select_done(n->pane);
          }
        }
        if (a->drag.kind == DRAG_TITLE &&
            (a->drag.tab_target || a->drag.new_tab_target)) {
          /* Dropped on the strip: the pane changes tab rather than places. */
          drop_pane_on_strip(a);
        } else if (a->drag.kind == DRAG_TITLE &&
                   a->drag.target != a->drag.src) {
          swap_panes(a, a->drag.src, a->drag.target);
        }
        a->drag.kind = DRAG_NONE;
        a->drag.src = a->drag.target = a->drag.tab_target = 0;
        a->drag.new_tab_target = false;
      }
      break; /* a drag owns the mouse until the button comes up */
    }

    /* Any press dismisses the cheatsheet, wherever it lands -- including on
       * its own close button, which is there because a modal without a way
       * out that you can *see* is a modal people hunt for the way out of. */
    if (a->help) {
      if (ev->maction == MOUSE_PRESS) a->help = false;
      break;
    }

    /* The finder owns the pointer as well as the keyboard. A press on one
       * of its rows chooses; a press anywhere else dismisses it and does
       * nothing further -- clicking past a modal must not also land on the
       * layout behind it, which is how you end up focusing a pane you were
       * trying to click *away* from. Motion still falls through, so rows
       * light up under the pointer. */
    if (a->picker && ev->maction != MOUSE_MOTION) {
      bool own = action && (strncmp(action, "find:", 5) == 0 ||
                            strncmp(action, "run:", 4) == 0 ||
                            strncmp(action, "open:", 5) == 0 ||
                            strcmp(action, "closefind") == 0 ||
                            strcmp(action, "closepalette") == 0 ||
                            strcmp(action, "closeprojects") == 0);
      if (!own) {
        if (ev->maction == MOUSE_PRESS) a->picker = PICK_NONE;
        break;
      }
    }

    if (action) do_action(a, action, ev);
    break;
  }
  case EV_PASTE:
    pane_send_paste(cur(a)->focus->pane, ev->paste, ev->paste_len);
    break;
  default: break;
  }
}

/* ---- dumping a session back out as a layout ------------------------------
 *
 * The inverse of apply-layout, and the thing that makes a restart survivable:
 * build a fresh binary, dump what you have, quit, come back with `--layout`.
 * contrib/sl0ppty-dev wraps exactly that.
 *
 * What can honestly be restored is the *shape*: tabs, their names and
 * purposes, how the panes are split, in what proportion, in which directory,
 * running what they were started with. What cannot is the state inside a
 * program -- a shell's history, a running vim -- and this does not pretend
 * otherwise. A pane running the session's default shell is dumped as a pane
 * with no command, so it comes back as a shell rather than as a re-run of one.
 */

typedef struct {
  char *buf;
  size_t len, cap;
} strbuf_t;

static void sb_add(strbuf_t *b, const char *fmt, ...) {
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
    b->cap = b->cap ? b->cap * 2 : 1024;
    while (b->cap - b->len <= (size_t)n) b->cap *= 2;
    b->buf = realloc(b->buf, b->cap);
  }
}

/* KDL strings are double-quoted, so the two characters that end or escape one
 * have to be escaped themselves. A path can contain both. */
static void sb_quoted(strbuf_t *b, const char *key, const char *val) {
  if (!val || !*val) return;
  sb_add(b, " %s=\"", key);
  for (const char *p = val; *p; p++) {
    if (*p == '"' || *p == '\\')
      sb_add(b, "\\%c", *p);
    else if ((unsigned char)*p >= 0x20)
      sb_add(b, "%c", *p);
  }
  sb_add(b, "\"");
}

/* Where the program in the pane is *now*, which after any amount of `cd` is
 * not where it was started. The kernel knows; nothing else does. */
static const char *live_cwd(const pane_t *p, char *buf, size_t cap) {
  pid_t pid = pane_pid(p);
  if (pid > 0) {
    char link[64];
    snprintf(link, sizeof link, "/proc/%d/cwd", (int)pid);
    ssize_t n = readlink(link, buf, cap - 1);
    if (n > 0) {
      buf[n] = 0;
      return buf;
    }
  }
  return pane_start_cwd(p); /* not running, or not Linux: what it was given */
}

/* Everything one dump needs to know, so dump_node does not have to ask the app
 * which tab it is in the middle of. The focused pane belongs to *its* tab, not
 * to the one you happen to be looking at -- reading `cur(a)->focus` here meant
 * a dump of three tabs restored the focus of one. */
typedef struct {
  const tab_t *tab;
  const char *base;
  int suspend;
  size_t panes, suspended;
} dumpctx_t;

/* The command this pane is written back out with, or NULL for a plain shell.
 *
 * Two sources, in this order. The **label** is what a layout told this pane to
 * run, and it outranks everything: it survives the program exiting (D14), and
 * re-saving a project must not degrade `npm run dev` into whatever node's argv
 * happens to look like this minute. Failing that, **what the pane's terminal is
 * actually running** -- because a pane you split and typed a command into had
 * nothing to say for itself and came back as a bare shell, which made setting a
 * project up by hand and writing it down two different jobs instead of one.
 *
 * An ephemeral pane is a task that happened to be open when the dump was taken,
 * and is written with no command either way: restoring it would reopen somebody's
 * editor on a file they finished with. */
static const char *dump_command(const node_t *n, char *buf, size_t cap) {
  if (pane_ephemeral(n->pane)) return NULL;
  const char *label = pane_label(n->pane);
  if (label && *label) return label;
  return pane_foreground(n->pane, buf, cap);
}

/* Whether this pane is written as one that has not started yet.
 *
 * `as-is` is the honest answer for a session dump: what is suspended now is
 * suspended in the file. It is the wrong answer for a project's layout, where
 * the pane running the dev server you started this morning would start one on
 * every open -- which is the thing `suspended` exists to prevent. So a saved
 * project defaults to `commands`: a pane that has a command is written asleep, a
 * shell is not. Same distinction, and the same word, as `keep_dead`.
 *
 * "Has a command" is whatever the file is about to say, captured or declared, so
 * the two cannot disagree -- a pane written with a `command=` and no
 * `suspended=true` under this policy would start a dev server on every open. */
static bool dump_suspended(const char *command, const node_t *n, int policy) {
  switch (policy) {
  case DUMP_SUSPEND_NONE: return false;
  case DUMP_SUSPEND_ALL: return true;
  case DUMP_SUSPEND_COMMANDS: return command && *command;
  default: return pane_suspended(n->pane);
  }
}

/* `split=` belongs on the node that *has* the children -- that is where
 * build_pane() reads it -- and a tab's own props are the props of its root,
 * so a root split says so on the tab. Getting this backwards produces a file
 * that loads without complaint and rebuilds the wrong tree. */
static void dump_node(node_t *n, strbuf_t *b, int depth, dumpctx_t *ctx) {
  char pad[64];
  int p = depth * 4 < 60 ? depth * 4 : 60;
  memset(pad, ' ', (size_t)p);
  pad[p] = 0;

  sb_add(b, "%spane", pad);
  /* A weight is a share of the parent, so the root of a tab has none. */
  if (n->parent) sb_add(b, " weight=%d", n->weight);

  if (n->kind == NODE_SPLIT) {
    sb_add(b, " split=\"%s\" {\n", n->dir == SPLIT_ROWS ? "rows" : "cols");
    for (size_t i = 0; i < n->nkids; i++)
      dump_node(n->kids[i], b, depth + 1, ctx);
    sb_add(b, "%s}\n", pad);
    return;
  }

  ctx->panes++;
  char cwdbuf[4096];
  /* Relative to the project when there is one, so the file is the same file on
   * another machine. A directory outside the base stays absolute: it is not a
   * fact about the project. */
  sb_quoted(b, "cwd",
            path_relative(live_cwd(n->pane, cwdbuf, sizeof cwdbuf), ctx->base));
  char cmdbuf[4096];
  const char *command = dump_command(n, cmdbuf, sizeof cmdbuf);
  sb_quoted(b, "command", command);
  sb_quoted(b, "purpose", n->purpose);
  if (dump_suspended(command, n, ctx->suspend)) {
    sb_add(b, " suspended=true");
    ctx->suspended++;
  }
  if (n == ctx->tab->focus) sb_add(b, " focus=true");
  sb_add(b, "\n");
}

char *app_dump_layout(app_t *a, dump_layout_t *o) {
  dump_layout_t all = {0};
  if (!o) o = &all;
  o->tabs = o->panes = o->suspended = 0;

  strbuf_t b = {0};
  sb_add(&b, "layout {\n"); /* no timestamp: a layout that differs every time
                             * is a bad diff */
  for (size_t i = 0; i < a->ntabs; i++) {
    tab_t *t = &a->tabs[i];
    if (o->tab && t->id != o->tab) continue;
    dumpctx_t ctx = {.tab = t, .base = o->base, .suspend = o->suspend};
    sb_add(&b, "    tab");
    sb_quoted(&b, "name", t->name);
    /* The workspace's own purpose is derived from where the project is, so a
     * file that lives there does not repeat it -- and a copy of the project
     * elsewhere becomes its own workspace rather than claiming this one's. */
    if (!(o->for_project && strncmp(t->purpose, "project:", 8) == 0))
      sb_quoted(&b, "purpose", t->purpose);
    /* Which tab you were looking at is a fact about a session. Asked for one
     * tab, the answer is that tab, and `active` would be noise in a file
     * checked in beside a project. */
    if (!o->tab && i == a->cur) sb_add(&b, " active=true");
    /* The tab's props are its root's props, so a root that is a split says
     * which way it goes here rather than on a `pane` node of its own. */
    if (t->root && t->root->kind == NODE_SPLIT)
      sb_add(&b, " split=\"%s\"", t->root->dir == SPLIT_ROWS ? "rows" : "cols");
    sb_add(&b, " {\n");
    if (t->root) {
      if (t->root->kind == NODE_SPLIT)
        for (size_t k = 0; k < t->root->nkids; k++)
          dump_node(t->root->kids[k], &b, 2, &ctx);
      else
        dump_node(t->root, &b, 2, &ctx);
    }
    sb_add(&b, "    }\n");
    o->tabs++;
    o->panes += ctx.panes;
    o->suspended += ctx.suspended;
  }
  sb_add(&b, "}\n");
  return b.buf;
}

/* ---- introspection ------------------------------------------------------ */

struct panes_json {
  app_t *a;
  json_t *j;
};
static void panes_cb(node_t *n, void *ud) {
  struct panes_json *pj = ud;
  json_t *j = pj->j;
  json_obj_open(j, NULL);
  json_int(j, "id", n->id);
  json_int(j, "x", n->rect.x);
  json_int(j, "y", n->rect.y);
  json_int(j, "w", n->rect.w);
  json_int(j, "h", n->rect.h);
  json_int(j, "content_x", n->content.x);
  json_int(j, "content_y", n->content.y);
  json_int(j, "content_w", n->content.w);
  json_int(j, "content_h", n->content.h);
  json_bool(j, "focused", n == cur(pj->a)->focus);
  json_bool(j, "hidden", n->hidden);
  json_bool(j, "suspended", pane_suspended(n->pane));
  /* A dead pane is still a pane, so tooling has to be able to tell: `alive`
   * false with a status is one whose program is over and which is waiting to
   * be re-run or closed. `exit_code` is -1 when there is no status to give. */
  json_bool(j, "alive", pane_alive(n->pane));
  {
    int code = 0;
    bool sig = false;
    bool known = pane_exit(n->pane, &code, &sig);
    json_int(j, "exit_code", known && !sig ? code : -1);
    json_int(j, "exit_signal", known && sig ? code : 0);
  }
  json_str(j, "purpose", n->purpose, strlen(n->purpose));
  json_bool(j, "purpose_declared", n->purpose_locked);
  /* Both, because they answer different questions and only one of them is stable:
   * `tab` is where it sits in the strip, which is what a person reads, and `tab_id`
   * is what every command that takes a tab wants. They coincide until a tab is
   * removed, which is exactly when a script that guessed would be wrong. */
  size_t ti = tab_of(pj->a, n);
  json_int(j, "tab", (long long)ti + 1);
  json_int(j, "tab_id", (long long)pj->a->tabs[ti].id);
  const char *t = pane_title(n->pane);
  json_str(j, "title", t ? t : "", t ? strlen(t) : 0);
  json_obj_close(j);
}

char *app_panes_json(app_t *a) {
  json_t j;
  json_init(&j);
  json_arr_open(&j, NULL);
  struct panes_json pj = {a, &j};
  walk_all(a, panes_cb, &pj); /* every tab, so tooling can find any pane */
  json_arr_close(&j);
  return j.buf;
}

char *app_tabs_json(app_t *a) {
  json_t j;
  json_init(&j);
  json_arr_open(&j, NULL);
  for (size_t i = 0; i < a->ntabs; i++) {
    tab_t *t = &a->tabs[i];
    size_t panes = 0;
    walk(t->root, count_cb, &panes);
    json_obj_open(&j, NULL);
    json_int(&j, "id", t->id);
    json_int(&j, "index", (long long)i + 1);
    json_str(&j, "name", t->name, strlen(t->name));
    json_str(&j, "purpose", t->purpose, strlen(t->purpose));
    json_bool(&j, "purpose_declared", t->purpose_locked);
    json_bool(&j, "active", i == a->cur);
    json_int(&j, "panes", (long long)panes);
    json_obj_close(&j);
  }
  json_arr_close(&j);
  return j.buf;
}
