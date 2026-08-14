/* The layout tree, focus, chrome, and what keys do. See app.h. */
#define _GNU_SOURCE
#include "app.h"

#include <ghostty/vt.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "graphics.h"
#include "kdl.h"

/* ---- config ------------------------------------------------------------- */

#include "config.h"

/* One config per process. The server owns the session, and a session has one
 * look; `reload` re-reads it in place, which is why nothing caches a colour. */
static config_t CFG;
static bool CFG_LOADED = false;

static void ensure_config(void) {
  if (CFG_LOADED) return;
  config_defaults(&CFG);
  char err[256] = {0};
  const char *path = config_default_path();
  if (!config_load(&CFG, path, err, sizeof err)) {
    /* A missing file is the normal case; a broken one is worth a line in the
     * log, and in both the compiled-in defaults stand (fail open). */
    if (access(path, R_OK) == 0)
      fprintf(stderr, "sl0ppty: %s: %s\n", path, err[0] ? err : "parse error");
  } else if (err[0]) {
    fprintf(stderr, "sl0ppty: %s: %s\n", path, err);
  }
  CFG_LOADED = true;
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
  return true;
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

static const color_t NO_COLOR = {0};

/* ---- tree --------------------------------------------------------------- */

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

  /* Colour passes over this pane's contents, applied in order. Attached by
   * policy inside this file; nothing in-band or over the control API can set
   * them yet, so a program cannot restyle itself by accident. */
  shader_t shaders[SHADE_MAX];
  size_t nshaders;
  char purpose[64];
  bool purpose_locked; /* declared by a layout: in-band cannot override */

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
  uint32_t h_id;      /* the rows split: this one moves vertically */
  size_t h_edge;
  uint32_t v_id[2];   /* the cols splits above and below: horizontally */
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
  bool prefix;
  bool quit;
  bool detach;
  /* One drag machine, two verbs: a title drags a pane onto another to swap
   * them, a gap between panes drags the boundary. Both are started by a press
   * on something the hit list says is draggable, so neither can disagree with
   * what is on screen. */
  struct {
    enum { DRAG_NONE, DRAG_TITLE, DRAG_EDGE, DRAG_SELECT, DRAG_BORDER } kind;
    uint32_t src;      /* pane being dragged, or the split being resized */
    uint32_t target;   /* pane under the pointer, for the drop highlight */
    size_t edge;       /* which boundary of that split */
    uint16_t x, y;     /* where the pointer was at the last event */
    bool moved;        /* a press that never moves is a click */
    char side;         /* border press: 'l' 'r' 't' 'b' */
    /* A corner drag carries the boundaries it is moving, not an index into
     * the corner list: that list is rebuilt from the layout every frame, and
     * the layout is the thing being changed. Halfway through a drag the
     * crossing you grabbed may be a different entry, or gone — an index would
     * quietly start moving somebody else's panes. */
    uint32_t c_h;
    size_t c_hedge;
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

  /* the pane finder overlay: tabs stop being navigation past about six */
  bool finder;
  char query[64];
  size_t sel;

  /* Renaming a pane in place: the title cell becomes the editor, so the name
   * is typed where it will live rather than in a dialog somewhere else. */
  bool renaming;
  uint32_t rename_id;
  /* As wide as a pane title, so seeding the editor never truncates one (and so
   * never truncates one mid-UTF-8). */
  char rename_buf[256];
  /* The first click of a candidate double-click on a title. */
  int64_t name_click_ms;
  uint32_t name_click_id;
  const char *const *argv;
  /* What this session is called. The server knows it because it opened the
   * socket under that name; the app only knows it because it is worth saying
   * out loud when several are running. */
  char session[64];
  /* the screen we last composed into: its hit list is what a click resolves
   * against, so routing can never consult geometry the user never saw */
  corner_t corners[16];
  size_t ncorners;
  const screen_t *painted;
};

static tab_t *cur(app_t *a) { return &a->tabs[a->cur]; }

/* Is the pointer sitting on this rect right now?
 *
 * Always asked with the rect that is about to be registered as the hit, so a
 * thing that lights up and the thing that would be clicked cannot drift apart.
 * A pointer already carrying something is busy and lights nothing. */
/* Display columns of a short chrome string. Counts codepoints, which is exact
 * for what chrome draws: every glyph here is one column, and a mark wide
 * enough not to be is the thing the config warns you about. */
static uint16_t cells(const char *str) {
  uint16_t n = 0;
  for (const char *q = str; q && *q; q++)
    if (((unsigned char)*q & 0xC0) != 0x80) n++;
  return n;
}

static bool ptr_on(const app_t *a, uint16_t x, uint16_t y, uint16_t w,
                   uint16_t h) {
  return a->ptr_valid && a->drag.kind == DRAG_NONE && a->ptr_x >= x &&
         a->ptr_x < x + w && a->ptr_y >= y && a->ptr_y < y + h;
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
static void on_pane_osc(pane_t *p, const char *verb, const char *payload,
                        void *ud) {
  app_t *a = ud;
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

/* When something needs repainting on its own: a toast expiring, or a hover
 * guide arming. Without the second one the guide would appear on the next
 * event rather than when the dwell is up, which for a resting pointer means
 * "never". */
int app_next_deadline_ms(app_t *a) {
  int64_t soonest = -1;
  for (size_t i = 0; i < a->ntoasts; i++)
    if (soonest < 0 || a->toasts[i].until < soonest) soonest = a->toasts[i].until;

  if (a->ptr_valid && a->painted) {
    int64_t due = a->ptr_still_since + CFG.hover_delay_ms;
    if (due > now_ms_()) {
      /* Everything that arms on dwell has to be listed here, or it would only
       * appear when some unrelated event happened to repaint the frame. */
      const char *action = hit_test(&a->painted->hits, a->ptr_x, a->ptr_y);
      bool on_border = action && (strncmp(action, "border:", 7) == 0 ||
                                  strncmp(action, "title:", 6) == 0 ||
                                  strncmp(action, "edge:", 5) == 0 ||
                                  strncmp(action, "corner:", 7) == 0);
      if (on_border && (soonest < 0 || due < soonest)) soonest = due;
    }
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
  if (strncmp(action, "scrollbottom:", 13) == 0) return "back to the bottom";
  if (strncmp(action, "panetitle:", 10) == 0)
    return "double-click to rename \u00b7 drag to move";
  if (strncmp(action, "title:", 6) == 0) return "drag to move \u00b7 click to split up";
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
  if (strncmp(action, "tab:", 4) == 0) return "switch to this tab";
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
  x = (uint16_t)(x + screen_text(s, x, bar.y, CFG.min_mark, MINBAR, NO_COLOR, 0));
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
      right = (uint16_t)(right - cw - 2);
    }
  }

  char ind[64] = {0};
  if (f) {
    if (app_pane_zoomed(a, f->id)) {
      snprintf(ind, sizeof ind, "zoomed");
    } else if (pane_suspended(f->pane)) {
      snprintf(ind, sizeof ind, "not started");
    } else if (!pane_alive(f->pane)) {
      snprintf(ind, sizeof ind, "exited");
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
      n += (size_t)snprintf(line + n, sizeof line - n, "%s%s", n ? " \u00b7 " : "", pp);
    else if (pt && *pt)
      n += (size_t)snprintf(line + n, sizeof line - n, "%s%s", n ? " \u00b7 " : "", pt);
  }
  if (n > (size_t)(right - x)) line[right - x] = 0;
  if (n) screen_text(s, x, y, line, STATUS_C, NO_COLOR, 0);

  /* The hint sits in what is left between the two ends, centred there rather
   * than in the row: centring it in the row would put it under the session
   * name on a narrow screen, and a hint that overwrites what it is explaining
   * is worse than no hint. It is simply not drawn when it does not fit. */
  if (!CFG.hints || !a->painted) return;
  const char *hint = hint_for(a, hit_test(&s->hits, a->ptr_x, a->ptr_y));
  if (!hint || !a->ptr_valid) return;

  uint16_t used = (uint16_t)(x + cells(line));
  uint16_t from = (uint16_t)(used + 2);
  if (right <= from) return;
  uint16_t span = (uint16_t)(right - from);
  uint16_t hw = cells(hint);
  if (hw + 2 > span) return;
  screen_text(s, (uint16_t)(from + (span - hw) / 2), y, hint, HINT_C, NO_COLOR,
              0);
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
    snprintf(msg, sizeof msg, "%s", (title && *title) ? title : body ? body : "");
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
  return n;
}

static node_t *leaf_new(app_t *a) {
  pane_t *p = pane_new(a->argv, 1, 1, NULL);
  if (!p) return NULL;
  node_t *n = calloc(1, sizeof *n);
  n->kind = NODE_LEAF;
  n->weight = WEIGHT_UNIT;
  n->pane = p;
  n->id = ++a->next_id;
  pane_set_osc_handler(p, on_pane_osc, a);
  pane_set_clipboard_handler(p, on_pane_clipboard, a);
  pane_set_notify_handler(p, on_pane_notify, a);
  return n;
}

static void node_free(node_t *n) {
  if (!n) return;
  if (n->kind == NODE_LEAF) {
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
static void collect_cb(node_t *n, void *ud) {
  struct collect *c = ud;
  if (c->n < c->max) c->out[c->n++] = pane_fd(n->pane);
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
  pane_pump(f.found->pane);
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
    /* content is the frame deflated by its border and padding; a rect too
     * small for a frame gets none, and the pane takes the whole thing. */
    uint16_t bx = r.w >= 3 && r.h >= 3 ? 1 + CFG.pad * CFG.gap_aspect : 0;
    uint16_t by = r.w >= 3 && r.h >= 3 ? 1 + CFG.pad : 0;
    n->content = (rect_t){
        .x = (uint16_t)(r.x + bx),
        .y = (uint16_t)(r.y + by),
        .w = (uint16_t)(r.w > 2 * bx ? r.w - 2 * bx : 1),
        .h = (uint16_t)(r.h > 2 * by ? r.h - 2 * by : 1),
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

  uint16_t gap = n->dir == SPLIT_COLS ? (uint16_t)(CFG.gap * CFG.gap_aspect)
                                      : CFG.gap;
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

/* ---- tree edits --------------------------------------------------------- */

static void replace_child(node_t *parent, node_t *old, node_t *new_) {
  for (size_t i = 0; i < parent->nkids; i++)
    if (parent->kids[i] == old) {
      parent->kids[i] = new_;
      new_->parent = parent;
      return;
    }
}

static void split_node(app_t *a, node_t *leaf, split_dir_t dir, bool before) {
  if (!leaf) return;
  node_t *fresh = leaf_new(a);
  if (!fresh) return;

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
    p->kids[slot] = fresh;
    fresh->parent = p;
    p->nkids++;
  } else {
    node_t *sp = calloc(1, sizeof *sp);
    sp->kind = NODE_SPLIT;
    sp->id = ++a->next_id;
    sp->weight = leaf->weight; /* the new split inherits the pane's share */
    leaf->weight = WEIGHT_UNIT;
    sp->dir = dir;
    sp->nkids = 2;
    sp->kids = malloc(2 * sizeof *sp->kids);
    sp->kids[before ? 1 : 0] = leaf;
    sp->kids[before ? 0 : 1] = fresh;
    sp->parent = leaf->parent;
    if (leaf->parent) replace_child(leaf->parent, leaf, sp);
    else cur(a)->root = sp;
    leaf->parent = sp;
    fresh->parent = sp;
  }

  cur(a)->focus = fresh;
  layout(a);
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
  return total >= (uint16_t)(k * floor_ + gap * (k - 1));
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
    if (p->parent) replace_child(p->parent, p, survivor);
    else t->root = survivor;
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
static void reap_cb(node_t *n, void *ud) {
  struct reap *r = ud;
  if (!r->dead && !pane_alive(n->pane)) r->dead = n;
}

void app_reap(app_t *a) {
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
    if (a->ntabs == 0) a->quit = true;
    else layout(a);
    return true;
  }
  return false;
}

/* D8's trust model: a purpose declared by a layout outranks an in-band one and
 * cannot be overridden, so `cat hostile.txt` in a pane cannot relabel a
 * project tab. `declared` is only ever true on the control path. */
bool app_set_pane_purpose(app_t *a, uint32_t id, const char *purpose,
                          bool declared) {
  struct byid b = {id, NULL};
  walk_all(a, byid_cb, &b);
  if (!b.found) return false;
  if (b.found->purpose_locked && !declared) return false;
  sanitise_purpose(purpose, b.found->purpose, sizeof b.found->purpose);
  if (declared) b.found->purpose_locked = true;
  return true;
}

bool app_set_tab_purpose(app_t *a, uint32_t id, const char *purpose,
                         bool declared) {
  for (size_t i = 0; i < a->ntabs; i++) {
    tab_t *t = &a->tabs[i];
    if (t->id != id) continue;
    if (t->purpose_locked && !declared) return false;
    sanitise_purpose(purpose, t->purpose, sizeof t->purpose);
    if (declared) t->purpose_locked = true;
    return true;
  }
  return false;
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

bool app_close_pane(app_t *a, uint32_t id) {
  node_t *n = id ? pane_by_id(a, id) : cur(a)->focus;
  if (!n) return false;
  close_leaf(a, n);
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
 */

static node_t *build_pane(app_t *a, const kdl_node_t *node, const char *cwd) {
  const char *node_cwd = kdl_prop(node, "cwd", cwd);

  /* a split: children, in order, in one direction */
  size_t kids = 0;
  for (size_t i = 0; i < node->nkids; i++)
    if (strcmp(node->kids[i]->name, "pane") == 0) kids++;

  if (kids) {
    node_t *sp = calloc(1, sizeof *sp);
    sp->kind = NODE_SPLIT;
    sp->id = ++a->next_id;
    sp->weight = WEIGHT_UNIT;
    sp->dir = strcmp(kdl_prop(node, "split", "cols"), "rows") == 0 ? SPLIT_ROWS
                                                                   : SPLIT_COLS;
    for (size_t i = 0; i < node->nkids; i++) {
      if (strcmp(node->kids[i]->name, "pane") != 0) continue;
      node_t *kid = build_pane(a, node->kids[i], node_cwd);
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
  bool suspended = kdl_prop_bool(node, "suspended", false);
  const char *argv[4];
  if (command) {
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = command;
    argv[3] = NULL;
  } else {
    argv[0] = a->argv[0];
    argv[1] = NULL;
    for (size_t i = 1; a->argv[i] && i < 3; i++) argv[i] = a->argv[i];
  }

  node_t *leaf = leaf_new_ex(a, command ? argv : a->argv, node_cwd, suspended,
                             command ? command : "");
  if (!leaf) return NULL;
  const char *purpose = kdl_prop(node, "purpose", NULL);
  if (purpose) {
    sanitise_purpose(purpose, leaf->purpose, sizeof leaf->purpose);
    leaf->purpose_locked = true; /* declared by a layout: in-band cannot win */
  }
  return leaf;
}

bool app_apply_layout(app_t *a, const kdl_node_t *root, bool replace, char *err,
                      size_t errcap) {
  const kdl_node_t *lay = kdl_child(root, "layout");
  if (!lay) lay = root; /* allow a bare list of tabs */

  size_t before = a->ntabs;
  size_t made = 0;
  for (size_t i = 0; i < lay->nkids; i++) {
    const kdl_node_t *t = lay->kids[i];
    if (strcmp(t->name, "tab") != 0) continue;

    tab_t *tab = tab_add(a, kdl_prop(t, "name", ""));
    const char *purpose = kdl_prop(t, "purpose", NULL);
    if (purpose) {
      sanitise_purpose(purpose, tab->purpose, sizeof tab->purpose);
      tab->purpose_locked = true;
    }

    /* the tab body is a split of its pane children */
    kdl_node_t body = {.name = (char *)"pane", .kids = t->kids,
                       .nkids = t->nkids, .props = t->props,
                       .nprops = t->nprops};
    node_t *tree = build_pane(a, &body, kdl_prop(t, "cwd", NULL));
    if (!tree) tree = leaf_new(a);
    if (!tree) {
      a->ntabs--;
      if (err) snprintf(err, errcap, "cannot create panes for tab %zu", i + 1);
      return false;
    }
    tab->root = tree;
    tab->focus = first_leaf_of(tree);
    made++;
  }

  if (!made) {
    if (err) snprintf(err, errcap, "layout declares no tabs");
    return false;
  }

  if (replace) { /* drop the tabs that existed before this layout */
    for (size_t i = 0; i < before; i++) node_free(a->tabs[i].root);
    memmove(&a->tabs[0], &a->tabs[before], made * sizeof *a->tabs);
    a->ntabs = made;
  }
  a->cur = replace ? 0 : before;
  layout(a);
  return true;
}

bool app_apply_layout_text(app_t *a, const char *text, bool replace, char *err,
                           size_t errcap) {
  kdl_node_t *root = kdl_parse(text, err, errcap);
  if (!root) return false;
  bool ok = app_apply_layout(a, root, replace, err, errcap);
  kdl_free(root);
  return ok;
}

bool app_apply_layout_file(app_t *a, const char *path, bool replace, char *err,
                           size_t errcap) {
  kdl_node_t *root = kdl_parse_file(path, err, errcap);
  if (!root) return false;
  bool ok = app_apply_layout(a, root, replace, err, errcap);
  kdl_free(root);
  return ok;
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
    uint16_t gap = sp->dir == SPLIT_COLS ? (uint16_t)(CFG.gap * CFG.gap_aspect)
                                         : CFG.gap;
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
    if (dir > 0) transfer_weight(p->kids[i + 1], n, WEIGHT_STEP);
    else transfer_weight(n, p->kids[i + 1], WEIGHT_STEP);
  } else if (i > 0) {
    if (dir > 0) transfer_weight(n, p->kids[i - 1], WEIGHT_STEP);
    else transfer_weight(p->kids[i - 1], n, WEIGHT_STEP);
  }
  layout(a);
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
  long fx = d->from->rect.x * 2 + d->from->rect.w, fy = d->from->rect.y * 2 + d->from->rect.h;
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

/* OSC 5577: a pane's own status line and buttons, drawn in its bottom frame
 * row. Buttons are budgeted from the right *before* the status text gets any
 * columns, and each registers its hit as it is painted — the same rule the
 * split button follows, for the same reason. */
static void draw_pane_status(screen_t *s, node_t *leaf, color_t fg,
                             bool focused) {
  const pane_button_t *btns = NULL;
  size_t nbtn = pane_buttons(leaf->pane, &btns);
  const char *status = pane_status(leaf->pane);
  if (!nbtn && !*status) return;

  rect_t r = leaf->rect;
  if (r.w < 6 || r.h < 3) return;
  uint16_t y = (uint16_t)(r.y + r.h - 1);
  uint16_t left = (uint16_t)(r.x + 1), right = (uint16_t)(r.x + r.w - 1);

  /* right to left, so a button that does not fit is simply not drawn */
  uint16_t x = right;
  for (size_t i = nbtn; i-- > 0;) {
    uint16_t w = (uint16_t)(strlen(btns[i].label) + 2); /* [label] */
    if (x < left + w + 1) break;
    x = (uint16_t)(x - w - 1);
    char label[80];
    snprintf(label, sizeof label, "[%s]", btns[i].label);
    uint16_t drawn = screen_text(s, x, y, label, focused ? BTN_FG : fg,
                                 focused ? BTN_BG : BTN_BG_IDLE, 0);
    char action[48];
    snprintf(action, sizeof action, "btn:%u:%s", leaf->id, btns[i].id);
    hit_add(&s->hits, x, y, drawn, 1, action);
  }

  if (*status && x > left + 1) {
    char buf[256];
    int len = snprintf(buf, sizeof buf, " %s ", status);
    uint16_t room = (uint16_t)(x - left);
    if (len > (int)room) {
      len = room;
      buf[len] = 0;
    }
    screen_text(s, left, y, buf, focused ? TITLE_FOCUS : TITLE_IDLE,
                NO_COLOR, 0);
  }
}

/* The split guide: the armed edge goes heavy, and a dashed line shows where
 * the new boundary would land. Drawn *after* the pane's content, because the
 * dashed line crosses it — the first version was painted under the terminal
 * and was invisible. Hover only, so an idle frame stays quiet. */
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
  char want[48];
  snprintf(want, sizeof want, "border:%u:", leaf->id);
  if (strncmp(action, want, strlen(want)) == 0) {
    side = action[strlen(want)];
  } else {
    snprintf(want, sizeof want, "title:%u", leaf->id);
    if (strcmp(action, want) == 0) side = 't';
  }
  if (!side) return;

  /* Nothing is offered that cannot be delivered: below the floor this pane
   * would only collapse, so the border simply stops being a button. */
  if (!split_fits(leaf, side_dir(side))) return;

  rect_t r = leaf->rect;
  if (r.w < 4 || r.h < 4) return;
  uint16_t x1 = (uint16_t)(r.x + r.w - 1), y1 = (uint16_t)(r.y + r.h - 1);
  color_t hi = GUIDE;

  /* An arrow on the new boundary, pointing at the side the new pane will take.
   * The dashed line says where, and on its own leaves which half is the new
   * one to be inferred from which border you happen to be touching.
   *
   * Pointers rather than the matching triangles for left and right: U+25C0 and
   * U+25B6 carry emoji presentation and terminals widely render them
   * double-width, and screen_text books every chrome glyph as one cell — so a
   * cell that draws as two would shift the rest of the row. U+25B2 is already
   * the scroll indicator, so the vertical pair is known good here. */
  if (side == 'l' || side == 'r') {
    uint16_t bx = side == 'l' ? r.x : x1;
    uint16_t mid = (uint16_t)(r.x + r.w / 2);
    for (uint16_t y = (uint16_t)(r.y + 1); y < y1; y++) {
      screen_text(s, bx, y, "\u2503", hi, NO_COLOR, ATTR_BOLD);
      screen_text(s, mid, y, "\u254e", hi, NO_COLOR, 0);
    }
    screen_text(s, mid, (uint16_t)(r.y + r.h / 2),
                side == 'l' ? "\u25c4" : "\u25ba", hi, NO_COLOR, ATTR_BOLD);
  } else {
    uint16_t by = side == 't' ? r.y : y1;
    uint16_t mid = (uint16_t)(r.y + r.h / 2);
    for (uint16_t x = (uint16_t)(r.x + 1); x < x1; x++) {
      screen_text(s, x, by, "\u2501", hi, NO_COLOR, ATTR_BOLD);
      screen_text(s, x, mid, "\u254c", hi, NO_COLOR, 0);
    }
    screen_text(s, (uint16_t)(r.x + r.w / 2), mid,
                side == 't' ? "\u25b2" : "\u25bc", hi, NO_COLOR, ATTR_BOLD);
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

  /* The frame's top row is the drag handle. Registered before the split
   * button, which is painted after and therefore wins its own cell. */
  {
    char action[48];
    snprintf(action, sizeof action, "title:%u", leaf->id);
    hit_add(&s->hits, r.x, r.y, r.w, 1, action);
  }

  /* No split button. The border *is* the button: clicking an edge splits
   * toward it, which encodes the direction a single glyph never could, and
   * gives every frame its columns back. */
  uint16_t avail = (uint16_t)(r.w - 2);
  bool has_btn = false;
  uint16_t btn_x = x1;

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
  }

  {
    char action[48];
    snprintf(action, sizeof action, "border:%u:l", leaf->id);
    hit_add(&s->hits, r.x, (uint16_t)(r.y + 1), 1, (uint16_t)(r.h - 2), action);
    snprintf(action, sizeof action, "border:%u:r", leaf->id);
    hit_add(&s->hits, x1, (uint16_t)(r.y + 1), 1, (uint16_t)(r.h - 2), action);
    snprintf(action, sizeof action, "border:%u:b", leaf->id);
    hit_add(&s->hits, r.x, y1, r.w, 1, action);
  }

  draw_pane_status(s, leaf, fg, focused);

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
      uint16_t drawn = screen_text(s, ix, r.y, ind, SCROLL_FG, SCROLL_BG, ATTR_BOLD);
      char action[48];
      snprintf(action, sizeof action, "scrollbottom:%u", leaf->id);
      hit_add(&s->hits, ix, r.y, drawn, 1, action);
      avail = (uint16_t)(avail - iw);
    }
  }

  const char *title = pane_title(leaf->pane);
  bool editing = a->renaming && a->rename_id == leaf->id;
  if ((editing || (title && *title)) && avail >= 3) {
    char buf[320]; /* a full-length name, plus the caret and its spaces */
    int len = editing ? snprintf(buf, sizeof buf, " %s\u2588 ", a->rename_buf)
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
    uint16_t drawn =
        screen_text(s, tx, r.y, buf,
                    editing ? RENAME_FG : (focused ? TITLE_FOCUS : TITLE_IDLE),
                    editing ? RENAME_BG : NO_COLOR,
                    editing || focused ? ATTR_BOLD : 0);

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
    }
  }
}

struct draw {
  app_t *a;
  screen_t *s;
};

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
  color_t label = hot ? HEADER_HOVER_TITLE : HEADER;

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
}

/* ---- shaders ------------------------------------------------------------ */

bool app_shade_add(app_t *a, uint32_t pane_id, const char *kind, color_t color,
                   uint8_t amount) {
  node_t *n = pane_by_id(a, pane_id);
  if (!n || n->nshaders >= SHADE_MAX) return false;
  shader_t sh;
  if (!shader_make(&sh, kind, color, amount)) return false;
  n->shaders[n->nshaders++] = sh;
  return true;
}

void app_shade_clear(app_t *a, uint32_t pane_id) {
  node_t *n = pane_by_id(a, pane_id);
  if (n) n->nshaders = 0;
}

size_t app_shade_count(app_t *a, uint32_t pane_id) {
  node_t *n = pane_by_id(a, pane_id);
  return n ? n->nshaders : 0;
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
static pane_state_t pane_state(app_t *a, node_t *n) {
  /* Only once the pointer has actually moved: a press that turns out to be a
   * click would otherwise flash the whole session on its way to nothing. */
  if (a->drag.kind == DRAG_TITLE && a->drag.moved) {
    if (n->id == a->drag.src) return PSTATE_DRAGGING;
    if (n->id == a->drag.target) return PSTATE_DROP_HOVER;
    return PSTATE_DROP_TARGET;
  }
  if (pane_suspended(n->pane)) return PSTATE_SUSPENDED;
  if (pane_scrolled(n->pane)) return PSTATE_SCROLLED;
  if (n != cur(a)->focus) return PSTATE_UNFOCUSED;
  return PSTATE_COUNT;
}

static size_t policy_shaders(app_t *a, node_t *n, shader_t *out, size_t cap) {
  pane_state_t st = pane_state(a, n);
  if (st >= PSTATE_COUNT) return 0;
  size_t k = CFG.state_n[st];
  if (k > cap) k = cap;
  for (size_t i = 0; i < k; i++) out[i] = CFG.state_shaders[st][i];
  return k;
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
  for (size_t i = 0; i < n->nshaders && nc < SHADE_CHAIN_MAX; i++)
    chain[nc++] = n->shaders[i];
  nc += policy_shaders(a, n, &chain[nc], SHADE_CHAIN_MAX - nc);

  if (!nc) return;
  bool focused = n == cur(a)->focus;
  shade_ctx_t base = {
      .now_ms = now_ms_(),
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
  shade_apply(s, chain, nc, n->content.x, n->content.y, n->content.w,
              n->content.h, &base);
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

    bool active = a->drag.kind == DRAG_EDGE && a->drag.c_nv &&
                  a->drag.c_h == c->h_id && a->drag.c_hedge == c->h_edge;
    /* (C) The mark appears the moment the pointer is on it, before the dwell
     * that arms the two boundaries. A crossing is two cells wide and gives no
     * other sign it is anything: something has to say it is there, and the
     * ghost costs nothing if you were only passing through. */
    bool over = a->drag.kind == DRAG_NONE && ptr_on(a, c->r.x, c->r.y, c->r.w, c->r.h);
    if (!active && !over) continue;
    bool armed = active || now_ms_() - a->ptr_still_since >= CFG.hover_delay_ms;
    for (uint16_t x = c->r.x; x < c->r.x + c->r.w; x++)
      screen_text(s, x, c->r.y, armed ? (active ? "\u256c" : "\u253c") : "\u253c",
                  RESIZE_C, NO_COLOR, active ? ATTR_BOLD : 0);
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
        line[w - 2] = 0xe2; /* fall back to a plain dot rather than a cut UTF-8 */
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
   * is painted after, so it stays legible on top of a shaded pane. */
  shade_leaf(d->a, d->s, n);
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
  uint16_t x = CFG.status_pad;
  uint16_t y = CFG.gap;

  /* Right side first, so a long tab list can never eat the indicators — the
   * same budgeting rule as the split button and the OSC buttons. */
  uint16_t right = (uint16_t)(s->cols > CFG.status_pad ? s->cols - CFG.status_pad
                                                       : s->cols);
  char info[64];
  size_t np = app_pane_count(a);
  snprintf(info, sizeof info, "%zu pane%s", np, np == 1 ? "" : "s");
  uint16_t iw = (uint16_t)strlen(info);
  if (right > iw + 4) {
    screen_text(s, (uint16_t)(right - iw), y, info, TAB_COUNT, NO_COLOR, 0);
    right = (uint16_t)(right - iw - 1);
  }
  if (a->prefix && right > 5) { /* the prefix is a mode: say so */
    screen_text(s, (uint16_t)(right - 3), y, "C-a", PREFIX_FG, PREFIX_BG,
                ATTR_BOLD);
    right = (uint16_t)(right - 4);
  }

  for (size_t i = 0; i < a->ntabs && x < right; i++) {
    tab_t *t = &a->tabs[i];
    char label[80];
    const char *nm = t->name[0] ? t->name : (t->purpose[0] ? t->purpose : "");
    /* A pane that rang in a tab you are not looking at is invisible without
     * this, and that is the case the whole indicator exists for. */
    bool rang = CFG.bell_indicator && tab_has_bell(t);
    if (nm[0]) snprintf(label, sizeof label, " %zu:%s ", i + 1, nm);
    else snprintf(label, sizeof label, " %zu ", i + 1);

    bool active = i == a->cur;
    uint16_t attrs = active ? ATTR_BOLD : 0;
    /* Two independent signals: weight says which tab you are in, colour says
     * where the pointer is. Drawn once to learn the width, then again in the
     * hover colour if that width turns out to be under the pointer — which
     * costs a repaint of a few cells and guarantees the lit cells are the
     * registered ones. */
    uint16_t w = screen_text(s, x, y, label,
                             active ? TAB_ACTIVE_FG : TAB_IDLE,
                             active ? TAB_ACTIVE_BG : NO_COLOR, attrs);
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
    if (ptr_on(a, x, y, w, 1))
      screen_text(s, x, y, label, active ? TAB_ACTIVE_HOVER_FG : TAB_HOVER,
                  active ? TAB_ACTIVE_BG : NO_COLOR, attrs | ATTR_BOLD);
    char action[48];
    snprintf(action, sizeof action, "tab:%u", t->id);
    hit_add(&s->hits, x, y, w, 1, action);
    x = (uint16_t)(x + w);
  }

  /* Not a bare "+": the frame already has one, for splitting a pane. Two
   * verbs that look identical is a UI bug the fork shipped and noticed. */
  if (x + 6 < right) {
    uint16_t w = screen_text(s, x, y, " +tab ", TAB_IDLE, NO_COLOR, 0);
    if (ptr_on(a, x, y, w, 1))
      screen_text(s, x, y, " +tab ", TAB_HOVER, NO_COLOR, ATTR_BOLD);
    hit_add(&s->hits, x, y, w, 1, "newtab");
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

  /* Every place a column boundary runs into a row boundary is its own
   * crossing. Two of them that happen to line up are one crossing with two
   * boundaries to move; two that do not are two crossings, each moving its
   * own. Grouping by the columns they occupy gets both, where taking the first
   * and discarding anything unlike it only ever found the aligned case. */
  for (size_t i = 0; i < n; i++) {
    if (g[i].sp->dir != SPLIT_ROWS) continue;
    rect_t h = g[i].r;

    for (size_t j = 0; j < n; j++) {
      if (g[j].sp->dir != SPLIT_COLS) continue;
      rect_t v = g[j].r;
      if ((uint16_t)(v.y + v.h) != h.y && (uint16_t)(h.y + h.h) != v.y) continue;
      uint16_t x0 = v.x > h.x ? v.x : h.x;
      uint16_t xe = (uint16_t)(v.x + v.w) < (uint16_t)(h.x + h.w)
                        ? (uint16_t)(v.x + v.w)
                        : (uint16_t)(h.x + h.w);
      if (xe <= x0) continue;
      uint16_t w = (uint16_t)(xe - x0);

      corner_t *c = NULL;
      for (size_t k = 0; k < a->ncorners; k++) {
        corner_t *e = &a->corners[k];
        if (e->h_id == g[i].sp->id && e->h_edge == g[i].i && e->r.x == x0 &&
            e->r.w == w) {
          c = e;
          break;
        }
      }
      if (!c) {
        if (a->ncorners >= 16) break;
        c = &a->corners[a->ncorners++];
        *c = (corner_t){0};
        c->r = (rect_t){x0, h.y, w, h.h};
        c->h_id = g[i].sp->id;
        c->h_edge = g[i].i;
      }
      if (c->nv < 2) {
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
  if (c->h_id == id && c->h_edge == edge) return true;
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
  if (a->drag.kind == DRAG_EDGE && a->drag.c_nv) {
    if ((a->drag.c_h == split->id && a->drag.c_hedge == idx)) active = true;
    for (size_t k = 0; k < a->drag.c_nv; k++)
      if (a->drag.c_v[k] == split->id && a->drag.c_vedge[k] == idx) active = true;
  } else if (a->drag.kind == DRAG_NONE) {
    ci = corner_at(a, a->ptr_x, a->ptr_y);
  }
  if (ci >= 0 && (size_t)ci < a->ncorners &&
      corner_uses(&a->corners[ci], split->id, idx)) {
    if (false) active = true;
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
    while (i < nl && p[i] && tolower((unsigned char)p[i]) ==
                                 tolower((unsigned char)needle[i]))
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

static void draw_finder(app_t *a, screen_t *s) {
  find_entry_t entries[64];
  size_t n = finder_entries(a, entries, 64);
  if (a->sel >= n) a->sel = n ? n - 1 : 0;

  uint16_t w = (uint16_t)(s->cols > 60 ? 56 : (s->cols > 20 ? s->cols - 8 : 12));
  uint16_t rows = (uint16_t)(n > 10 ? 10 : (n ? n : 1));
  uint16_t h = (uint16_t)(rows + 4);
  if (h > s->rows) h = s->rows;
  uint16_t x = (uint16_t)((s->cols - w) / 2), y = (uint16_t)((s->rows - h) / 2);

  for (uint16_t yy = y; yy < y + h; yy++)
    for (uint16_t xx = x; xx < x + w; xx++)
      screen_text(s, xx, yy, " ", NO_COLOR, FINDER_BG, 0);

  char head[128];
  snprintf(head, sizeof head, " find: %s\u2588", a->query);
  screen_text(s, (uint16_t)(x + 1), (uint16_t)(y + 1), head, FINDER_FG,
              FINDER_BG, ATTR_BOLD);

  for (size_t i = 0; i < rows && i < n; i++) {
    size_t idx = i + (a->sel >= rows ? a->sel - rows + 1 : 0);
    if (idx >= n) break;
    find_entry_t *e = &entries[idx];
    char line[256];
    const char *title = pane_title(e->node->pane);
    snprintf(line, sizeof line, " %zu:%-12.12s %-16.16s %s",
             e->tab + 1,
             e->tab == (size_t)-1 ? "" : a->tabs[e->tab].name,
             title && *title ? title : "pane", e->node->purpose);
    line[w - 2 < sizeof line ? w - 2 : sizeof line - 1] = 0;
    bool on = idx == a->sel;
    uint16_t yy = (uint16_t)(y + 3 + i);
    uint16_t drawn = screen_text(s, (uint16_t)(x + 1), yy, line,
                                 on ? FINDER_SEL_FG : FINDER_FG,
                                 on ? FINDER_SEL_BG : FINDER_BG, 0);
    char action[48];
    snprintf(action, sizeof action, "find:%u", e->node->id);
    hit_add(&s->hits, (uint16_t)(x + 1), yy, drawn, 1, action);
  }
}

/* Returns true when the finder consumed the event. */
static bool finder_key(app_t *a, const input_event_t *ev) {
  if (!a->finder) return false;
  if (ev->kind != EV_KEY || ev->action == KEY_RELEASE) return true;

  switch (ev->key) {
    case GHOSTTY_KEY_ESCAPE:
      a->finder = false;
      return true;
    case GHOSTTY_KEY_ENTER: {
      find_entry_t entries[64];
      size_t n = finder_entries(a, entries, 64);
      if (a->sel < n) app_focus_pane(a, entries[a->sel].node->id);
      a->finder = false;
      return true;
    }
    case GHOSTTY_KEY_ARROW_DOWN:
      a->sel++;
      return true;
    case GHOSTTY_KEY_ARROW_UP:
      if (a->sel) a->sel--;
      return true;
    case GHOSTTY_KEY_BACKSPACE: {
      size_t l = strlen(a->query);
      if (l) a->query[l - 1] = 0;
      a->sel = 0;
      return true;
    }
    default:
      break;
  }
  if (ev->text_len && (unsigned char)ev->text[0] >= 0x20) {
    size_t l = strlen(a->query);
    if (l + ev->text_len < sizeof a->query) {
      memcpy(a->query + l, ev->text, ev->text_len);
      a->query[l + ev->text_len] = 0;
      a->sel = 0;
    }
  }
  return true;
}

/* ---- renaming a pane ---------------------------------------------------- */

/* The editor lives in the title cell itself (drawn by draw_frame), so the name
 * is typed where it is going to live. There is no dialog to place, nothing to
 * dismiss, and the pane it belongs to cannot be in doubt. */
static void rename_begin(app_t *a, uint32_t id) {
  node_t *n = pane_by_id(a, id);
  if (!n) return;
  a->renaming = true;
  a->rename_id = id;
  /* Seed with what is on screen: a rename is usually an edit, not a retype. */
  const char *shown = pane_title(n->pane);
  snprintf(a->rename_buf, sizeof a->rename_buf, "%s", shown ? shown : "");
  a->name_click_ms = 0;
  a->name_click_id = 0;
  app_focus_pane(a, id);
}

static void rename_end(app_t *a, bool keep) {
  if (!a->renaming) return;
  node_t *n = pane_by_id(a, a->rename_id);
  if (keep && n) {
    pane_set_name(n->pane, a->rename_buf);
    app_toast(a, a->rename_buf[0] ? "renamed" : "name cleared");
  }
  a->renaming = false;
  a->rename_id = 0;
  a->rename_buf[0] = 0;
}

/* Returns true when the rename editor consumed the event. */
static bool rename_key(app_t *a, const input_event_t *ev) {
  if (!a->renaming) return false;
  if (ev->kind != EV_KEY || ev->action == KEY_RELEASE) return true;

  switch (ev->key) {
    case GHOSTTY_KEY_ESCAPE:
      rename_end(a, false);
      return true;
    case GHOSTTY_KEY_ENTER:
      rename_end(a, true);
      return true;
    case GHOSTTY_KEY_BACKSPACE: {
      /* A character, not a byte: a title is whatever the program could set,
       * and half a UTF-8 sequence is not a name. */
      size_t l = strlen(a->rename_buf);
      while (l && ((unsigned char)a->rename_buf[l - 1] & 0xC0) == 0x80) l--;
      if (l) l--;
      a->rename_buf[l] = 0;
      return true;
    }
    default:
      break;
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
  if (a->finder) draw_finder(a, s); /* painted last, so its hits win */
  draw_toasts(a, s);                /* and above even that: it is transient */
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
  if (g->col + cols > leaf->content.w) {
    uint16_t over = (uint16_t)(g->col + cols - leaf->content.w);
    cols = (uint16_t)(cols - over);
    uint32_t px = over * g->cell_px_w;
    sw = sw > px ? sw - px : 0;
  }
  if (g->row + rows > leaf->content.h) {
    uint16_t over = (uint16_t)(g->row + rows - leaf->content.h);
    rows = (uint16_t)(rows - over);
    uint32_t px = over * g->cell_px_h;
    sh = sh > px ? sh - px : 0;
  }
  if (!cols || !rows) return;

  gfx_place(c->a->gfx, leaf->id, g->image_id, g->generation, g->place_id,
            (uint16_t)(leaf->content.x + g->col),
            (uint16_t)(leaf->content.y + g->row), cols, rows, g->sx, g->sy, sw,
            sh, g->src_w, g->src_h, g->format, g->compression, g->data,
            g->data_len);
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


  if (cells > 0) transfer_weight(sp->kids[i + 1], sp->kids[i], amount);
  else transfer_weight(sp->kids[i], sp->kids[i + 1], amount);
  layout(a);
}

/* Focus follows the mouse, but never at the cost of what you were doing.
 *
 * Hovering may not: steal focus mid-chord (the prefix is held), reach past an
 * open finder, interrupt a drag, or expand a collapsed pane just because the
 * pointer crossed its header — which is why only `pane:` and `title:` targets
 * count, and the frame rect's `focus:` does not. */
static bool hover_focus_allowed(const app_t *a) {
  return CFG.focus_follows_mouse && !a->prefix && !a->finder && !a->renaming &&
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
        if (a->name_click_id == id &&
            now - a->name_click_ms <= (int64_t)CFG.double_click_ms) {
          rename_begin(a, id);
          return; /* a rename, not a grab: start no drag */
        }
        a->name_click_ms = now;
        a->name_click_id = id;
      }
      /* The top border is both a drag handle and an edge: whether this is a
       * move or a split-upward is decided by whether the pointer moves. The
       * title text is only a handle, so it takes no side: a click that never
       * moved does nothing there instead of splitting upward. */
      a->drag.kind = DRAG_TITLE;
      a->drag.src = id;
      a->drag.target = id;
      a->drag.x = ev->mx;
      a->drag.y = ev->my;
      a->drag.moved = false;
      a->drag.side = on_name ? 0 : 't';
      app_focus_pane(a, id);
    }
    return;
  }
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
    a->drag.c_h = c->h_id;
    a->drag.c_hedge = c->h_edge;
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
  if (strncmp(action, "find:", 5) == 0) {
    if (ev->maction != MOUSE_PRESS) return;
    app_focus_pane(a, (uint32_t)strtoul(action + 5, NULL, 10));
    a->finder = false;
    return;
  }
  if (strncmp(action, "tab:", 4) == 0) {
    if (ev->maction == MOUSE_PRESS)
      app_select_tab_id(a, (uint32_t)strtoul(action + 4, NULL, 10));
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
  } else if (strncmp(action, "scrollbottom:", 13) == 0) {
    if (ev->maction == MOUSE_PRESS) pane_scroll_edge(n->pane, false);
  } else if (strncmp(action, "pane:", 5) == 0) {
    /* A press always focuses; a hover only when it is allowed to. The event
     * is forwarded either way, so a pane that tracks the mouse still sees the
     * pointer cross it whether or not focus moved. */
    if (ev->maction != MOUSE_MOTION || hover_focus_allowed(a)) cur(a)->focus = n;
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
      pane_select_start(n->pane, local.mx, local.my);
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
  action_t act = config_lookup(&CFG, ev->key, ev->mods);
  if (act >= ACT_SELECT_TAB_1) {
    app_select_tab(a, (size_t)(act - ACT_SELECT_TAB_1));
    return true;
  }
  switch (act) {
    case ACT_SPLIT_COLS: split_focus_ui(a, SPLIT_COLS); return true;
    case ACT_SPLIT_ROWS: split_focus_ui(a, SPLIT_ROWS); return true;
    case ACT_ZOOM: app_toggle_zoom(a, 0); return true;
    case ACT_MINIMIZE:
      if (!app_minimize(a, 0)) app_toast(a, "nothing else to show");
      return true;
    case ACT_CLOSE_PANE:
      if (cur(a)->focus) close_leaf(a, cur(a)->focus);
      return true;
    case ACT_FOCUS_LEFT: focus_dir(a, -1, 0); return true;
    case ACT_FOCUS_RIGHT: focus_dir(a, 1, 0); return true;
    case ACT_FOCUS_UP: focus_dir(a, 0, -1); return true;
    case ACT_FOCUS_DOWN: focus_dir(a, 0, 1); return true;
    case ACT_FOCUS_NEXT: focus_next(a); return true;
    case ACT_NEW_TAB: app_new_tab(a, ""); return true;
    case ACT_NEXT_TAB: app_cycle_tab(a, 1); return true;
    case ACT_PREV_TAB: app_cycle_tab(a, -1); return true;
    case ACT_RESIZE_LEFT: resize_focus(a, -1, 0); return true;
    case ACT_RESIZE_RIGHT: resize_focus(a, 1, 0); return true;
    case ACT_RESIZE_UP: resize_focus(a, 0, -1); return true;
    case ACT_RESIZE_DOWN: resize_focus(a, 0, 1); return true;
    case ACT_SCROLL_UP: pane_scroll(cur(a)->focus->pane, -CFG.scroll_lines); return true;
    case ACT_SCROLL_DOWN: pane_scroll(cur(a)->focus->pane, CFG.scroll_lines); return true;
    case ACT_SCROLL_PAGE_UP:
      pane_scroll(cur(a)->focus->pane, -(int)cur(a)->focus->content.h);
      return true;
    case ACT_SCROLL_PAGE_DOWN:
      pane_scroll(cur(a)->focus->pane, (int)cur(a)->focus->content.h);
      return true;
    case ACT_SCROLL_TOP: pane_scroll_edge(cur(a)->focus->pane, true); return true;
    case ACT_SCROLL_BOTTOM: pane_scroll_edge(cur(a)->focus->pane, false); return true;
    case ACT_FINDER:
      a->finder = true;
      a->query[0] = 0;
      a->sel = 0;
      return true;
    case ACT_DETACH: a->detach = true; return true;
    case ACT_QUIT: a->quit = true; return true;
    default: return false;
  }
}

void app_event(app_t *a, const input_event_t *ev) {
  if (!a->ntabs || !cur(a)->focus) return;

  /* A rename whose pane went away must not keep the keyboard. */
  if (a->renaming && !pane_by_id(a, a->rename_id)) rename_end(a, false);

  /* The rename editor and the finder each own the keyboard while open; the
   * mouse still routes through the hit list, whose topmost entries are the
   * finder's own rows. */
  if (a->renaming && ev->kind == EV_KEY) {
    rename_key(a, ev);
    return;
  }
  if (a->finder && ev->kind == EV_KEY) {
    finder_key(a, ev);
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
      if (is_prefix || config_lookup(&CFG, ev->key, mods) == ACT_LITERAL_PREFIX) {
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
       * Clicking the pane's own name again does not, so a stray second
       * double-click lands in the editor instead of closing it. */
      if (a->renaming && ev->maction == MOUSE_PRESS) {
        char own[48];
        snprintf(own, sizeof own, "panetitle:%u", a->rename_id);
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
          } else if (a->drag.kind == DRAG_EDGE && a->drag.c_nv) {
            /* One axis to each: the row boundary takes the vertical movement
             * and the column boundaries the horizontal, so the corner follows
             * the pointer in both at once. The column boundaries above and
             * below get the same delta, which is what keeps them one line. */
            int dx = (int)ev->mx - (int)a->drag.x;
            int dy = (int)ev->my - (int)a->drag.y;
            drag_edge(a, split_by_id(a, a->drag.c_h), a->drag.c_hedge, dy);
            for (size_t i = 0; i < a->drag.c_nv; i++)
              drag_edge(a, split_by_id(a, a->drag.c_v[i]), a->drag.c_vedge[i],
                        dx);
          } else if (a->drag.kind == DRAG_EDGE) {
            node_t *sp = split_by_id(a, a->drag.src);
            int cells = sp && sp->dir == SPLIT_COLS
                            ? (int)ev->mx - (int)a->drag.x
                            : (int)ev->my - (int)a->drag.y;
            drag_edge(a, sp, a->drag.edge, cells);
          } else if (action && strncmp(action, "title:", 6) == 0) {
            a->drag.target = (uint32_t)strtoul(action + 6, NULL, 10);
          } else if (action && strncmp(action, "panetitle:", 10) == 0) {
            /* Dropping onto a pane's name is dropping onto that pane. */
            a->drag.target = (uint32_t)strtoul(action + 10, NULL, 10);
          } else if (action && strncmp(action, "pane:", 5) == 0) {
            a->drag.target = (uint32_t)strtoul(action + 5, NULL, 10);
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
          if (a->drag.kind == DRAG_TITLE && a->drag.target != a->drag.src)
            swap_panes(a, a->drag.src, a->drag.target);
          a->drag.kind = DRAG_NONE;
          a->drag.src = a->drag.target = 0;
        }
        break; /* a drag owns the mouse until the button comes up */
      }

      if (action) do_action(a, action, ev);
      break;
    }
    case EV_PASTE:
      pane_send_paste(cur(a)->focus->pane, ev->paste, ev->paste_len);
      break;
    default:
      break;
  }
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
  json_str(j, "purpose", n->purpose, strlen(n->purpose));
  json_bool(j, "purpose_declared", n->purpose_locked);
  json_int(j, "tab", (long long)tab_of(pj->a, n) + 1);
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
