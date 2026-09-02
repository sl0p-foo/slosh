/* The session core: config in force, pane and tab lifecycle, tree walking,
 * input routing, and introspection. See app.h for the public surface, and
 * app_internal.h for what the split shares -- layout and tree edits live in
 * app_layout.c, layout files and workspaces in app_session.c, drawing and
 * shading in app_draw.c, pickers and modals in app_ui.c. */
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
config_t CFG;
static bool CFG_LOADED = false;
/* What the config in force complained about while loading, or "". A complaint
 * is not a failure: an include that is not there, a shader nobody has heard of,
 * a binding that does not parse — the rest of the file applied and the session
 * is running (D9). But dropping it on the floor is how a mistyped theme name
 * turns into ten minutes of wondering, so it is kept for whoever can say it out
 * loud. The front end toasts it; the log gets it either way. */
static char CFG_COMPLAINT[256];

void ensure_config(void) {
  if (CFG_LOADED) return;
  config_defaults(&CFG);
  char err[256] = {0};
  const char *path = config_default_path();
  if (!config_load(&CFG, path, err, sizeof err)) {
    /* A missing file is the normal case; a broken one is worth a line in the
     * log, and in both the compiled-in defaults stand (fail open). */
    if (access(path, R_OK) == 0) {
      fprintf(stderr, "slosh: %s: %s\n", path, err[0] ? err : "parse error");
      snprintf(CFG_COMPLAINT, sizeof CFG_COMPLAINT, "%s",
               err[0] ? err : "config parse error");
    }
  } else if (err[0]) {
    fprintf(stderr, "slosh: %s: %s\n", path, err);
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

bool app_cfg_multi_attach(void) {
  ensure_config();
  return CFG.multi_attach;
}

bool app_cfg_attach_indicator(void) {
  ensure_config();
  return CFG.attach_indicator;
}

void app_cfg_attach_colors(color_t *fg, color_t *bg) {
  ensure_config();
  if (fg) *fg = CFG.attach_fg;
  if (bg) *bg = CFG.attach_bg;
}

int app_cfg_size_follows(void) {
  ensure_config();
  return CFG.size_follows;
}

/* ---- tree --------------------------------------------------------------- */
#include "app_internal.h"

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
uint16_t cells(const char *str) { return screen_cells(str); }

bool ptr_on(const app_t *a, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  return a->ptr_valid && a->drag.kind == DRAG_NONE && a->ptr_x >= x &&
         a->ptr_x < x + w && a->ptr_y >= y && a->ptr_y < y + h;
}

/* "exited", "exited: status 3", "exited: signal 9" — the same words wherever
 * the fact is reported, so the status line, the pane's own frame and the line
 * left in its backlog cannot describe one death three ways. */
void exit_words(const pane_t *p, char *out, size_t cap) {
  int code = 0;
  bool sig = false;
  if (!pane_exit(p, &code, &sig) || (!sig && code == 0))
    snprintf(out, cap, "exited");
  else
    snprintf(out, cap, "exited: %s %d", sig ? "signal" : "status", code);
}

typedef void (*leaf_fn_fwd)(node_t *, void *);
void walk_all(app_t *a, leaf_fn_fwd fn, void *ud);

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

int64_t now_ms_(void) {
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

  /* A selection drag holding past the pane's top or bottom edge scrolls on a
   * clock, not on motion: a pointer resting there sends no events, and "keep
   * going" is exactly what resting it there means. */
  if (a->drag.kind == DRAG_SELECT && a->ptr_valid) {
    node_t *n = pane_by_id(a, a->drag.src);
    if (n && ((int)a->ptr_y < (int)n->content.y ||
              (int)a->ptr_y >= (int)n->content.y + (int)n->content.h)) {
      int64_t due = a->drag.scroll_due;
      int64_t now = now_ms_();
      if (due < now) due = now;
      if (soonest < 0 || due < soonest) soonest = due;
    }
  }

  if (soonest < 0) return -1;
  int64_t in = soonest - now_ms_();
  return in <= 0 ? 0 : (int)in;
}

/* Extend a pane's selection toward the pointer, clamped into its content:
 * the nearest cell is what a pointer past an edge is pointing at. */
static void select_extend_to_pointer(app_t *a, node_t *n) {
  if (!n->content.w || !n->content.h) return;
  int lx = (int)a->ptr_x - (int)n->content.x;
  int ly = (int)a->ptr_y - (int)n->content.y;
  lx = lx < 0 ? 0 : lx >= (int)n->content.w ? n->content.w - 1 : lx;
  ly = ly < 0 ? 0 : ly >= (int)n->content.h ? n->content.h - 1 : ly;
  pane_select_extend(n->pane, (uint16_t)lx, (uint16_t)ly);
}

/* The auto-scroll step behind app_next_deadline_ms's DRAG_SELECT deadline.
 * Rows per step is how far past the edge the pointer sits -- one row past
 * creeps, further hurries -- which is rate control by the hand that is
 * already there. The step scrolls, then re-extends the selection: the anchor
 * is a tracked grid ref (pane.c), so only the near end moves. True when
 * anything changed, so a caller knows a repaint is owed. */
bool app_tick(app_t *a) {
  if (a->drag.kind != DRAG_SELECT || !a->ptr_valid) return false;
  node_t *n = pane_by_id(a, a->drag.src);
  if (!n || !n->content.h) return false;
  int ly = (int)a->ptr_y - (int)n->content.y;
  int over = ly < 0                    ? ly
             : ly >= (int)n->content.h ? ly - (int)n->content.h + 1
                                       : 0;
  if (!over) return false;
  int64_t now = now_ms_();
  if (now < a->drag.scroll_due) return false;
  /* Advance the clock whether or not the scroll had anywhere to go: a
   * pointer parked past an edge of a fully scrolled pane must not turn the
   * deadline into a busy loop. */
  a->drag.scroll_due = now + CFG.select_scroll_ms;
  pane_scroll(n->pane, over);
  select_extend_to_pointer(a, n);
  return true;
}

size_t app_toast_count(app_t *a) {
  toasts_expire(a);
  return a->ntoasts;
}

size_t count_leaves(node_t *n); /* all defined with the layout */
size_t collect_minimized(node_t *n, node_t **out, size_t cap, size_t k);
size_t collect_leaves(node_t *n, node_t **out, size_t cap, size_t k);

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
  if (strncmp(action, "title:", 6) == 0) {
    /* A float's top row only moves it; the click-to-split half of the
     * handle's promise is exactly what a float does not offer. */
    node_t *n = pane_by_id(a, id);
    if (n && n->floating) return "drag to move";
    /* The handle says both things it can do; the rest of the row only drags. */
    return strchr(action + 6, ':') ? "drag to move \u00b7 click to split up"
                                   : "drag to move";
  }
  if (strncmp(action, "brim:", 5) == 0 || strncmp(action, "border:", 7) == 0) {
    /* On a float the whole border is the resize grab, corners both ways —
     * read from the same derivation the press uses, so the caption and the
     * drag cannot disagree. */
    node_t *n = pane_by_id(a, id);
    if (n && n->floating) {
      if (!a->ptr_valid) return "drag to resize";
      uint8_t e = float_edges_at(n, a->ptr_x, a->ptr_y);
      return (e & (FEDGE_L | FEDGE_R)) && (e & (FEDGE_T | FEDGE_B))
                 ? "drag to resize both ways"
                 : "drag to resize";
    }
    /* The rim gets no caption on purpose: it is the part of the edge that
     * does nothing, and the guide it arms is already pointing at the part
     * that does. */
    if (action[1] == 'r') return NULL; /* brim: */
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
void draw_min_bar(app_t *a, screen_t *s) {
  if (!a->ntabs) return;
  rect_t bar = cur(a)->min_bar;
  if (!bar.w || !bar.h) return;

  node_t *mins[64];
  size_t nmin = collect_minimized(cur(a)->root, mins, 64, 0);
  if (!nmin) return;

  uint16_t x = bar.x;
  uint16_t right = (uint16_t)(bar.x + bar.w);

  /* Each entry is a miniature pane frame: three rows, the same corners a
   * real pane wears. A frame is what a pane *looks like* in this program,
   * so a put-away pane wearing one reads as a pane you can click — a row of
   * dim bare names read as a caption, and nobody clicks a caption; brackets
   * were tried and still read as text. The put-away mark rides inside each
   * chip, saying what kind of pane this is, and a rung pane's bell mark
   * keeps its own colour inside the frame: a pane you cannot see is exactly
   * the one a bell is for. The whole chip is the hit. */
  const char *tl = CFG.rounded ? "\u256d" : "\u250c",
             *tr = CFG.rounded ? "\u256e" : "\u2510";
  const char *bl = CFG.rounded ? "\u2570" : "\u2514",
             *br = CFG.rounded ? "\u256f" : "\u2518";

  for (size_t i = 0; i < nmin && x < right; i++) {
    const char *nm = pane_title(mins[i]->pane);
    bool rang = CFG.bell_indicator && pane_bell(mins[i]->pane);

    char label[96];
    snprintf(label, sizeof label, " %s %s", CFG.min_mark,
             nm && *nm ? nm : "pane");
    uint16_t lw = cells(label);
    uint16_t bw = rang ? (uint16_t)(1 + cells(CFG.bell_mark)) : 0;
    uint16_t inner = (uint16_t)(lw + bw + 1); /* and a breath before the wall */
    uint16_t chip = (uint16_t)(inner + 2);    /* the frame's two columns */
    if ((uint16_t)(x + chip) > right) break;

    bool hot = ptr_on(a, x, bar.y, chip, 3);
    color_t fg = hot ? MINBAR_HOVER : MINBAR;
    uint16_t attrs = hot ? ATTR_BOLD : 0;
    uint16_t x1 = (uint16_t)(x + chip - 1);
    uint16_t ymid = (uint16_t)(bar.y + 1), ybot = (uint16_t)(bar.y + 2);

    screen_text(s, x, bar.y, tl, fg, NO_COLOR, attrs);
    screen_text(s, x1, bar.y, tr, fg, NO_COLOR, attrs);
    screen_text(s, x, ybot, bl, fg, NO_COLOR, attrs);
    screen_text(s, x1, ybot, br, fg, NO_COLOR, attrs);
    for (uint16_t cx = (uint16_t)(x + 1); cx < x1; cx++) {
      screen_text(s, cx, bar.y, "\u2500", fg, NO_COLOR, attrs);
      screen_text(s, cx, ybot, "\u2500", fg, NO_COLOR, attrs);
    }
    screen_text(s, x, ymid, "\u2502", fg, NO_COLOR, attrs);
    screen_text(s, x1, ymid, "\u2502", fg, NO_COLOR, attrs);
    uint16_t w =
        screen_text(s, (uint16_t)(x + 1), ymid, label, fg, NO_COLOR, attrs);
    if (rang) {
      char mark[40];
      snprintf(mark, sizeof mark, " %s", CFG.bell_mark);
      w = (uint16_t)(w + screen_text(s, (uint16_t)(x + 1 + w), ymid, mark,
                                     BELL_C, NO_COLOR, ATTR_BOLD));
    }
    for (uint16_t cx = (uint16_t)(x + 1 + w); cx < x1; cx++)
      screen_text(s, cx, ymid, " ", fg, NO_COLOR, 0);

    char action[48];
    snprintf(action, sizeof action, "focus:%u", mins[i]->id);
    hit_add(&s->hits, x, bar.y, chip, 3, action);
    x = (uint16_t)(x + chip + 1);
  }
}

void draw_status_line(app_t *a, screen_t *s) {
  if (!CFG.status_line || s->rows < 3) return;
  /* Compact rides the very bottom row: the gap it floats on is air, and
   * compact has none. */
  uint16_t y = (uint16_t)(s->rows - (CFG.compact ? 0 : CFG.gap) - 1);
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
   * and when there is none, which slosh this is. The hint wins because it is
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
    middle = "slosh " SLOSH_VERSION;
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

void draw_toasts(app_t *a, screen_t *s) {
  toasts_expire(a);
  if (!a->ntoasts) return;
  uint16_t bottom = (uint16_t)(s->rows > 1 ? s->rows - 1 : 0);
  for (size_t i = 0; i < a->ntoasts; i++) {
    const char *text = a->toasts[a->ntoasts - 1 - i].text;
    char line[160];
    snprintf(line, sizeof line, " %s ", text);
    uint16_t w = (uint16_t)strlen(line);
    /* Too wide: elide the middle rather than the end. Almost every toast that
     * overflows is about a path, and a path's last component is the part you
     * needed -- clipping the tail told you "cannot open /var/folders/5q/g_q5"
     * and made you go and look for the rest. */
    uint16_t avail = (uint16_t)(s->cols > 1 ? s->cols - 1 : 1);
    if (w > avail && avail >= 5) { /* room for a head, the dots and a tail */
      size_t head = (size_t)(avail - 3) / 2, tail = (size_t)avail - 3 - head;
      char elided[160];
      snprintf(elided, sizeof elided, "%.*s...%s", (int)head, line,
               line + strlen(line) - tail);
      memcpy(line, elided, sizeof line);
      w = avail;
    } else if (w > avail) {
      /* A terminal too narrow for the dots gets a plain clip: `avail - 3`
       * below five goes negative, and negative as size_t is a pointer into
       * the weeds. */
      w = avail;
    }
    uint16_t y = (uint16_t)(bottom - i);
    if (y >= s->rows) break;
    /* Clamped, not wrapped: a toast wider than the screen minus the gap used
     * to compute a negative x, which as uint16_t became ~65533 and drew the
     * whole message off-screen -- the long messages, which are the ones you
     * most want to read, were the ones that vanished. */
    int xi = (int)s->cols - (int)w - (int)(CFG.gap * CFG.gap_aspect);
    uint16_t x = xi > 0 ? (uint16_t)xi : 0;
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

node_t *leaf_new_ex(app_t *a, const char *const argv[], const char *cwd,
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
const char *const *default_argv(app_t *a) {
  static const char *argv[2];
  if (a->argv && a->argv[0]) return a->argv;
  const char *sh = CFG.shell && *CFG.shell ? CFG.shell : getenv("SHELL");
#ifdef _WIN32
  /* Windows sets no SHELL; ComSpec is the equivalent and respects a user who
   * has pointed it somewhere else. */
  if (!sh || !*sh) sh = getenv("ComSpec");
#endif
  argv[0] = sh && *sh ? sh : SLOSH_SHELL_DEFAULT;
  argv[1] = NULL;
  return argv;
}

/* Where the program in a pane *is*, which after any amount of `cd` is not where
 * it was started. Defined with the dumper, declared here because a split wants
 * it too: a new pane opens where the one it came out of stands. */
const char *live_cwd(const pane_t *p, char *buf, size_t cap);

/* A plain pane running the session's shell. One line, because everything a pane
 * needs setting up is in leaf_new_ex and a second copy of that list is a second
 * place to forget something -- which is exactly how panes came to have every
 * handler attached and no history limit. */
node_t *leaf_new(app_t *a) {
  return leaf_new_ex(a, default_argv(a), NULL, false, "");
}

/* Empty a chain, freeing the programs it owned. */
void chain_clear(inband_chain_t *c) {
  for (size_t i = 0; i < c->nexprs; i++) expr_free(c->exprs[i]);
  c->nexprs = 0;
  c->n = 0;
}

void node_free(node_t *n) {
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
void sanitise_purpose(const char *in, char *out, size_t cap) {
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

tab_t *tab_add(app_t *a, const char *name) {
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

void app_cancel_client_pointer(app_t *a) {
  if (a->drag.kind == DRAG_SELECT) {
    node_t *n = pane_by_id(a, a->drag.src);
    if (n) pane_select_done(n->pane);
  }
  memset(&a->drag, 0, sizeof a->drag);
  a->ptr_valid = false;
}

void app_cancel_client_interaction(app_t *a) {
  a->prefix = false;
  app_cancel_client_pointer(a);
}

/* ---- walking ------------------------------------------------------------ */

typedef void (*leaf_fn)(node_t *, void *);

void walk(node_t *n, leaf_fn fn, void *ud) {
  if (!n) return;
  if (n->kind == NODE_LEAF) {
    fn(n, ud);
    return;
  }
  for (size_t i = 0; i < n->nkids; i++) walk(n->kids[i], fn, ud);
}

void walk_all(app_t *a, leaf_fn fn, void *ud) {
  for (size_t i = 0; i < a->ntabs; i++) walk(a->tabs[i].root, fn, ud);
}

/* Which tab a node lives in: climb to its root and match. */
size_t tab_of(app_t *a, node_t *n) {
  while (n->parent) n = n->parent;
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].root == n) return i;
  return (size_t)-1;
}

void byid_cb(node_t *n, void *ud) {
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

/* The split boundary a pane's side sits on, walking up as far as it takes:
 * the pane's parent may divide the other way, and the boundary to the right
 * of a pane can belong to a split any number of levels up. False for a side
 * on the outer edge, where there is nothing to drag against.
 *
 * This is what lets a *border* drag resize. With `gap 0` the panes are flush
 * and there is no gap cell to grab — gap_rect answers false, so no resize
 * handle is ever registered — and the border is all there is. The border was
 * already a button (a click splits toward it); motion turning the same press
 * into a boundary drag gives the two gestures one target, and a press that
 * never moves still splits exactly as it did. */
static bool boundary_for_side(node_t *leaf, char side, node_t **sp_out,
                              size_t *edge_out) {
  split_dir_t dir = side_dir(side);
  bool before = side == 'l' || side == 't';
  for (node_t *n = leaf; n && n->parent; n = n->parent) {
    node_t *p = n->parent;
    if (p->dir != dir) continue; /* divides the other way: keep climbing */
    size_t i = 0;
    while (i < p->nkids && p->kids[i] != n) i++;
    if (i >= p->nkids) return false;
    if (before ? i > 0 : i + 1 < p->nkids) {
      *sp_out = p;
      *edge_out = before ? i - 1 : i;
      return true;
    }
    /* At this split's extreme edge: the boundary, if any, is further up. */
  }
  return false;
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

/* Grab a float's edge: which edges follow the pointer is derived from where
 * the press landed on the rect painted this frame, so a bottom corner is two
 * edges and resizes on both axes — no corner target to register, because the
 * geometry that was just painted already says which corner it is. The same
 * derivation the hint and the guide read (float_edges_at), so the promise
 * and the grab cannot disagree. */
static void float_resize_press(app_t *a, node_t *n, const input_event_t *ev) {
  uint8_t e = float_edges_at(n, ev->mx, ev->my);
  if (!e) return;
  a->drag.kind = DRAG_FLOAT_RESIZE;
  a->drag.src = n->id;
  a->drag.fedges = e;
  a->drag.moved = false;
  a->drag.x = ev->mx;
  a->drag.y = ev->my;
  /* Grabbing a float raises it, like every window you have ever held. */
  app_focus_pane(a, n->id);
}

/* During a title drag, is this pane somewhere the drop could land? A float
 * is not: swapping tree seats with a pane that is not *in* its seat would
 * rearrange something invisible. 0 clears the highlight as well as the drop,
 * so hovering a float mid-drag promises nothing. */
static uint32_t swap_target(app_t *a, uint32_t id) {
  node_t *n = pane_by_id(a, id);
  return n && n->floating ? 0 : id;
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
      /* A float's top row moves it — its own verb, because everything a
       * title drag means on release (swap, split up) is what a float must
       * not do. The rename double-click above still wins, so a float's name
       * is still a name. */
      node_t *fn = pane_by_id(a, id);
      if (fn && fn->floating) {
        a->drag.kind = DRAG_FLOAT_MOVE;
        a->drag.src = id;
        a->drag.moved = false;
        a->drag.x = ev->mx;
        a->drag.y = ev->my;
        app_focus_pane(a, id);
        return;
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
      a->drag.drop_side = 0;
      a->drag.x = ev->mx;
      a->drag.y = ev->my;
      a->drag.moved = false;
      const char *sep = on_name ? NULL : strchr(action + 6, ':');
      a->drag.side = sep && sep[1] ? sep[1] : 0;
      a->drag.rim = false; /* the top row's handle really does split */
      app_focus_pane(a, id);
    }
    return;
  }
  /* The rim of an edge: hover arms the guide, and a *click* does nothing at
   * all. Deliberately inert and deliberately explicit -- this is the whole
   * point of the handle, and a silent fall-through to whatever comes next
   * would be a thin place for it to live.
   *
   * A press that then moves is a different gesture: the rim arms the same
   * border drag the handle does, marked as the rim's, so motion can turn it
   * into a boundary drag while its release stays the click that splits
   * nothing. Grabbing anywhere on a border and pulling moves the boundary,
   * which is the only way to move one when `gap 0` leaves no gap to grab.
   *
   * On a float the whole border is a resize grab instead: a float has no
   * guide to arm and no split to guard, and the rim being inert was about
   * accidents that rearrange a layout — resizing the thing you grabbed is
   * the accident-free reading of the same gesture. */
  if (strncmp(action, "brim:", 5) == 0 || strncmp(action, "border:", 7) == 0) {
    bool rim = action[1] == 'r'; /* brim */
    const char *rest = action + (rim ? 5 : 7);
    uint32_t id = (uint32_t)strtoul(rest, NULL, 10);
    const char *colon = strchr(rest, ':');
    char side = colon && colon[1] ? colon[1] : 'r';
    if (ev->maction == MOUSE_PRESS) {
      node_t *fn = pane_by_id(a, id);
      if (fn && fn->floating) {
        float_resize_press(a, fn, ev);
        return;
      }
      a->drag.kind = DRAG_BORDER;
      a->drag.src = id;
      a->drag.side = side;
      a->drag.rim = rim;
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
      a->drag.scroll_due = 0; /* the first edge contact may scroll at once */
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

/* Ctrl-D where the line discipline cannot do it: see `ctrl_d_exits` in
 * config.h for why this exists and why it is this narrow.
 *
 * What it sends is the word, not a signal. `exit` is what the user would have
 * typed and what every shell slosh can start already understands, so the pane
 * ends the way it ends on POSIX -- the shell's own cleanup, its own exit
 * status, and `keep_dead` deciding whether the corpse stays. Reaching past the
 * shell to close the pane would skip all three, and would also be a second
 * meaning for a key that already has one.
 *
 * Returns true when the keystroke was spent here and must not be forwarded. */
static bool ctrl_d_exit(app_t *a, const input_event_t *ev) {
  if (!CFG.ctrl_d_exits || ev->action == KEY_RELEASE) return false;
  if (ev->key != GHOSTTY_KEY_D) return false;
  if ((ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER | MOD_SHIFT)) != MOD_CTRL)
    return false;

  pane_t *p = cur(a)->focus->pane;
  /* An editor or a pager reads ^D as half a page down, and both live on the
   * alternate screen -- which is the one thing here the pane can state rather
   * than have guessed about it. */
  if (pane_alt_screen(p)) return false;
  /* A program in the foreground is a program that may want the EOF itself.
   * NULL is the shell at its own prompt, with nothing running under it. */
  char fg[1024];
  if (pane_foreground(p, fg, sizeof fg)) return false;
  /* On a line with something on it, POSIX sends EOF and the shell ignores it;
   * what it does not do is throw the line away. Neither do we. */
  if (!pane_line_empty(p)) return false;

  pane_write(p, "exit\r", 5);
  return true;
}

/* What an action does, given the action rather than the key that asked for it
 * -- because the command palette asks for actions by name, and a palette that
 * had to synthesise a keystroke to run one would be inventing input. */
bool run_action(app_t *a, action_t act) {
  if (act >= ACT_SELECT_TAB_1) {
    app_select_tab(a, (size_t)(act - ACT_SELECT_TAB_1));
    return true;
  }
  switch (act) {
  case ACT_SPLIT: split_focus_auto(a); return true;
  case ACT_SPLIT_COLS: split_focus_ui(a, SPLIT_COLS); return true;
  case ACT_SPLIT_ROWS:
    /* On a focused float, `-` shrinks it: the split it usually means is a
       * refusal there, and minus meaning smaller is the reading the hand
       * expects. The pair's other half, grow, has the =/+ key to itself. */
    if (focus_float_grow(a, -1)) return true;
    split_focus_ui(a, SPLIT_ROWS);
    return true;
  case ACT_ZOOM: app_toggle_zoom(a, 0); return true;
  case ACT_MINIMIZE:
    if (!app_minimize(a, 0)) app_toast(a, "nothing else to show");
    return true;
  case ACT_FLOAT:
    /* The refusal is the guard: floating the last tiled pane would leave
       * the overlay nothing to be over. */
    if (!app_toggle_float(a, 0)) app_toast(a, "nothing to float over");
    return true;
  case ACT_NEW_FLOAT:
    if (!app_new_float(a)) app_toast(a, "cannot open a pane");
    return true;
  case ACT_FLOAT_GROW:
    if (!focus_float_grow(a, 1)) app_toast(a, "nothing floating to grow");
    return true;
  case ACT_FLOAT_SHRINK:
    if (!focus_float_grow(a, -1)) app_toast(a, "nothing floating to shrink");
    return true;
  case ACT_CLOSE_PANE:
    if (cur(a)->focus) close_leaf(a, cur(a)->focus);
    return true;
  case ACT_SET_PURPOSE:
    /* Nothing to tag in an empty tab, and a purpose belongs to a pane rather
       * than to the space one would occupy. */
    if (cur(a)->focus) purpose_begin(a, cur(a)->focus->id);
    return true;
  case ACT_RENAME_PANE:
    if (cur(a)->focus) rename_begin(a, cur(a)->focus->id);
    return true;
  case ACT_RENAME_TAB:
    /* The editor draws in the tab's own cell in the strip; with the strip
       * off there is nowhere to type, and an invisible editor holding the
       * keyboard is the wedged state this program does not do. */
    if (!CFG.status_bar) {
      app_toast(a, "no tab strip to rename in");
      return true;
    }
    rename_tab_begin(a, cur(a)->id);
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
  /* On a focused float the same keys move the float: it has no boundary to
     * move, and moving the thing itself is the same verb aimed at what is
     * there. */
  case ACT_RESIZE_LEFT:
    if (!focus_float_move(a, -1, 0)) resize_focus(a, -1, 0);
    return true;
  case ACT_RESIZE_RIGHT:
    if (!focus_float_move(a, 1, 0)) resize_focus(a, 1, 0);
    return true;
  case ACT_RESIZE_UP:
    if (!focus_float_move(a, 0, -1)) resize_focus(a, 0, -1);
    return true;
  case ACT_RESIZE_DOWN:
    if (!focus_float_move(a, 0, 1)) resize_focus(a, 0, 1);
    return true;
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

void app_splash(app_t *a) { app_splash_fx(a, -1, -1); }

void app_splash_fx(app_t *a, int fx, int motion) {
  if (!CFG.splash_ms) return;
  a->splash_until = now_ms_() + CFG.splash_ms;
  a->splash_fx = fx;
  a->splash_motion = motion;
}

void app_event(app_t *a, const input_event_t *ev) {
  /* The splash is a greeting, not a modal: the first thing you do ends it,
   * and that thing still happens -- swallowing a keystroke someone aimed at
   * their shell would make the greeting a nuisance. */
  if (a->splash_until && (ev->kind == EV_KEY ||
                          (ev->kind == EV_MOUSE && ev->maction == MOUSE_PRESS)))
    a->splash_until = 0;

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
    a->drag.drop_side = 0;
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
    if (ctrl_d_exit(a, ev)) break;
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
        /* A border press that moves is a boundary drag, when the side has a
         * boundary. Converted on the first motion rather than at the press,
         * so a press that never moves is still the click that splits. */
        if (a->drag.kind == DRAG_BORDER) {
          node_t *bn = pane_by_id(a, a->drag.src);
          node_t *bsp = NULL;
          size_t bedge = 0;
          if (bn && !bn->floating &&
              boundary_for_side(bn, a->drag.side, &bsp, &bedge)) {
            a->drag.kind = DRAG_EDGE;
            a->drag.c_nv = a->drag.c_nh = 0;
            a->drag.src = bsp->id;
            a->drag.edge = bedge;
          }
        }
        if (a->drag.kind == DRAG_SELECT) {
          /* Clamped, not refused: the old unsigned subtraction underflowed
           * for a pointer above or left of the pane, and the selection froze
           * at the border. Past an edge the drag keeps the nearest cell, and
           * app_tick scrolls the viewport after it. */
          node_t *n = pane_by_id(a, a->drag.src);
          if (n) select_extend_to_pointer(a, n);
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
        } else if (a->drag.kind == DRAG_FLOAT_MOVE) {
          node_t *n = pane_by_id(a, a->drag.src);
          if (n)
            float_move(a, n, (int)ev->mx - (int)a->drag.x,
                       (int)ev->my - (int)a->drag.y);
        } else if (a->drag.kind == DRAG_FLOAT_RESIZE) {
          node_t *n = pane_by_id(a, a->drag.src);
          if (n)
            float_resize(a, n, a->drag.fedges, (int)ev->mx - (int)a->drag.x,
                         (int)ev->my - (int)a->drag.y);
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
          a->drag.drop_side = 0;
        } else if (a->drag.kind == DRAG_TITLE && action &&
                   strcmp(action, "newtab") == 0) {
          /* The button that makes a tab, used as somewhere to put one pane. */
          a->drag.new_tab_target = true;
          a->drag.tab_target = 0;
          a->drag.target = 0;
          a->drag.drop_side = 0;
        } else if (a->drag.kind == DRAG_TITLE && action &&
                   strncmp(action, "drop:", 5) == 0) {
          /* A drop zone: insert beside that pane, on that side. The zone
             * was registered by the paint the pointer is over, so it exists
             * exactly where the fill said it would. */
          a->drag.target = (uint32_t)strtoul(action + 5, NULL, 10);
          const char *sep = strrchr(action, ':');
          a->drag.drop_side = sep && sep[1] ? sep[1] : 0;
          a->drag.tab_target = 0;
          a->drag.new_tab_target = false;
        } else if (action && strncmp(action, "title:", 6) == 0) {
          a->drag.target =
              swap_target(a, (uint32_t)strtoul(action + 6, NULL, 10));
          a->drag.tab_target = 0;
          a->drag.new_tab_target = false;
          a->drag.drop_side = 0;
        } else if (action && strncmp(action, "panetitle:", 10) == 0) {
          /* Dropping onto a pane's name is dropping onto that pane. */
          a->drag.target =
              swap_target(a, (uint32_t)strtoul(action + 10, NULL, 10));
          a->drag.tab_target = 0;
          a->drag.new_tab_target = false;
          a->drag.drop_side = 0;
        } else if (action && strncmp(action, "pane:", 5) == 0) {
          a->drag.target =
              swap_target(a, (uint32_t)strtoul(action + 5, NULL, 10));
          a->drag.tab_target = 0;
          a->drag.new_tab_target = false;
          a->drag.drop_side = 0;
        }
        a->drag.x = ev->mx;
        a->drag.y = ev->my;
      } else if (ev->maction == MOUSE_RELEASE) {
        /* A press that never moved is a click, and a click on an edge
           * splits toward it. */
        if (!a->drag.moved && a->drag.side && !a->drag.rim &&
            (a->drag.kind == DRAG_BORDER || a->drag.kind == DRAG_TITLE)) {
          node_t *n = pane_by_id(a, a->drag.src);
          if (n) {
            char side = a->drag.side;
            bool before = side == 'l' || side == 't';
            split_dir_t dir = side_dir(side);
            /* The guide already declined to offer this, so the click that
               * the guide would have explained must decline too — otherwise
               * the border silently does something it just said it would not. */
            if (n->floating) { /* its own words: the room is not the problem */
              app_toast(a, "a floating pane cannot be split");
            } else if (!split_fits(n, dir)) {
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
        } else if (a->drag.kind == DRAG_TITLE && a->drag.moved && action &&
                   strncmp(action, "drop:", 5) == 0) {
          /* A zone inserts beside its pane, on its side: the drop grammar.
             * Resolved from the painted frame's hits at the moment of
             * release, not from a side remembered off the last motion —
             * the zones are painted one frame behind the pointer, and a
             * remembered answer could disagree with the fill on screen.
             * Derive from what the user saw; same lesson as the guide. */
          uint32_t dst = (uint32_t)strtoul(action + 5, NULL, 10);
          const char *sep = strrchr(action, ':');
          if (dst != a->drag.src && sep && sep[1])
            insert_pane_beside(a, a->drag.src, dst, sep[1]);
        } else if (a->drag.kind == DRAG_TITLE &&
                   a->drag.target != a->drag.src) {
          /* The centre is the swap it always was. */
          swap_panes(a, a->drag.src, a->drag.target);
        }
        a->drag.kind = DRAG_NONE;
        a->drag.src = a->drag.target = a->drag.tab_target = 0;
        a->drag.new_tab_target = false;
        a->drag.drop_side = 0;
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
  json_bool(j, "floating", n->floating);
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
