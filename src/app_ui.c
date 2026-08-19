/* The finder, modals, the palette, the project picker, renaming. Split from app.c. */
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
#include "app_internal.h"

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
bool push_pane_a_tab(app_t *a, bool forward) {
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
void drop_pane_on_strip(app_t *a) {
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

bool run_action(app_t *a, action_t act);

/* Do what a row says it does. One entry point for the keyboard and the mouse,
 * so a picker cannot choose one way with Enter and another with a click. */
void picker_accept(app_t *a, const char *action) {
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
bool picker_key(app_t *a, const input_event_t *ev) {
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
void rename_begin(app_t *a, uint32_t id) {
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
void purpose_begin(app_t *a, uint32_t id) {
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

void rename_tab_begin(app_t *a, uint32_t id) {
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

void rename_end(app_t *a, bool keep) {
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
bool rename_key(app_t *a, const input_event_t *ev) {
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
  draw_splash(a, s); /* the greeting outranks everything, briefly */
}
