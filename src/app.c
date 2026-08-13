/* The layout tree, focus, chrome, and what keys do. See app.h. */
#define _GNU_SOURCE
#include "app.h"

#include <ghostty/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

/* ---- config (compiled defaults for now; D2's parser lands in M3) -------- */

typedef struct {
  uint16_t gap;        /* blank rows between panes and at the screen edge */
  uint16_t gap_aspect; /* columns per row: cells are ~2x taller than wide */
  uint16_t pad;        /* rows/cols between a frame and its content */
  bool rounded;
  enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } title_align;
} config_t;

static const config_t CFG = {
    .gap = 1, .gap_aspect = 2, .pad = 0, .rounded = true,
    .title_align = ALIGN_CENTER,
};

static const color_t FRAME_FOCUS = {true, 0xff, 0x5f, 0xd7};
static const color_t FRAME_IDLE = {true, 0x45, 0x45, 0x4a};
static const color_t TITLE_FOCUS = {true, 0xff, 0xff, 0xff};
static const color_t NO_COLOR = {0};

/* ---- tree --------------------------------------------------------------- */

struct node {
  enum { NODE_LEAF, NODE_SPLIT } kind;
  node_t *parent;
  rect_t rect; /* recomputed every layout pass; never trusted between them */

  /* leaf */
  pane_t *pane;
  uint32_t id;
  rect_t content; /* where the pane's cells go */
  char purpose[64];
  bool purpose_locked; /* declared by a layout: in-band cannot override */

  /* split */
  split_dir_t dir;
  node_t **kids;
  size_t nkids;
};

/* A tab is a layout tree and its focus. Panes in every tab keep running; only
 * the current tab is composed. */
typedef struct {
  node_t *root;
  node_t *focus;
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
  const char *const *argv;
  /* the screen we last composed into: its hit list is what a click resolves
   * against, so routing can never consult geometry the user never saw */
  const screen_t *painted;
};

static tab_t *cur(app_t *a) { return &a->tabs[a->cur]; }

static node_t *leaf_new(app_t *a) {
  pane_t *p = pane_new(a->argv, 1, 1, NULL);
  if (!p) return NULL;
  node_t *n = calloc(1, sizeof *n);
  n->kind = NODE_LEAF;
  n->pane = p;
  n->id = ++a->next_id;
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
  app_t *a = calloc(1, sizeof *a);
  a->argv = argv;
  a->cols = cols;
  a->rows = rows;
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

static void layout_node(node_t *n, rect_t r) {
  n->rect = r;
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
    pane_resize(n->pane, n->content.w, n->content.h);
    return;
  }

  size_t k = n->nkids;
  uint16_t gap = n->dir == SPLIT_COLS ? (uint16_t)(CFG.gap * CFG.gap_aspect)
                                      : CFG.gap;
  uint16_t total = n->dir == SPLIT_COLS ? r.w : r.h;
  uint16_t gaps = (uint16_t)(gap * (k - 1));
  uint16_t avail = total > gaps ? (uint16_t)(total - gaps) : (uint16_t)k;
  uint16_t each = (uint16_t)(avail / k);
  uint16_t extra = (uint16_t)(avail % k); /* spread the remainder, no drift */

  uint16_t pos = n->dir == SPLIT_COLS ? r.x : r.y;
  for (size_t i = 0; i < k; i++) {
    uint16_t size = (uint16_t)(each + (i < extra ? 1 : 0));
    rect_t cr = n->dir == SPLIT_COLS
                    ? (rect_t){.x = pos, .y = r.y, .w = size, .h = r.h}
                    : (rect_t){.x = r.x, .y = pos, .w = r.w, .h = size};
    layout_node(n->kids[i], cr);
    pos = (uint16_t)(pos + size + gap);
  }
}

#define STRIP_ROWS 1 /* M5 turns this into the real status bar */

static void layout(app_t *a) {
  if (!a->ntabs) return;
  uint16_t gx = (uint16_t)(CFG.gap * CFG.gap_aspect), gy = CFG.gap;
  uint16_t top = (uint16_t)(gy + STRIP_ROWS);
  rect_t r = {.x = gx,
              .y = top,
              .w = (uint16_t)(a->cols > 2 * gx ? a->cols - 2 * gx : a->cols),
              .h = (uint16_t)(a->rows > top + gy ? a->rows - top - gy : 1)};
  if (cur(a)->root) layout_node(cur(a)->root, r);
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

static void split_focus(app_t *a, split_dir_t dir) {
  node_t *leaf = cur(a)->focus;
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
    memmove(&p->kids[at + 2], &p->kids[at + 1],
            (p->nkids - at - 1) * sizeof *p->kids);
    p->kids[at + 1] = fresh;
    fresh->parent = p;
    p->nkids++;
  } else {
    node_t *sp = calloc(1, sizeof *sp);
    sp->kind = NODE_SPLIT;
    sp->dir = dir;
    sp->nkids = 2;
    sp->kids = malloc(2 * sizeof *sp->kids);
    sp->kids[0] = leaf;
    sp->kids[1] = fresh;
    sp->parent = leaf->parent;
    if (leaf->parent) replace_child(leaf->parent, leaf, sp);
    else cur(a)->root = sp;
    leaf->parent = sp;
    fresh->parent = sp;
  }

  cur(a)->focus = fresh;
  layout(a);
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

static void draw_frame(app_t *a, screen_t *s, node_t *leaf) {
  rect_t r = leaf->rect;
  if (r.w < 3 || r.h < 3) return;
  bool focused = leaf == cur(a)->focus;
  color_t fg = focused ? FRAME_FOCUS : FRAME_IDLE;
  uint16_t attrs = 0;

  const char *tl = CFG.rounded ? "╭" : "┌", *tr = CFG.rounded ? "╮" : "┐";
  const char *bl = CFG.rounded ? "╰" : "└", *br = CFG.rounded ? "╯" : "┘";

  uint16_t x1 = (uint16_t)(r.x + r.w - 1), y1 = (uint16_t)(r.y + r.h - 1);
  screen_text(s, r.x, r.y, tl, fg, NO_COLOR, attrs);
  screen_text(s, x1, r.y, tr, fg, NO_COLOR, attrs);
  screen_text(s, r.x, y1, bl, fg, NO_COLOR, attrs);
  screen_text(s, x1, y1, br, fg, NO_COLOR, attrs);
  for (uint16_t x = (uint16_t)(r.x + 1); x < x1; x++) {
    screen_text(s, x, r.y, "─", fg, NO_COLOR, attrs);
    screen_text(s, x, y1, "─", fg, NO_COLOR, attrs);
  }
  for (uint16_t y = (uint16_t)(r.y + 1); y < y1; y++) {
    screen_text(s, r.x, y, "│", fg, NO_COLOR, attrs);
    screen_text(s, x1, y, "│", fg, NO_COLOR, attrs);
  }

  /* The split button is budgeted BEFORE the title, and hit-tested from the
   * same rect it is drawn in. Both halves of that sentence are scar tissue
   * from the sl0ppi fork, where a title long enough to fill the frame ate the
   * button's columns and the click landed two cells away from the glyph. */
  uint16_t avail = (uint16_t)(r.w - 2);
  uint16_t btn_x = 0;
  bool has_btn = avail >= 6;
  if (has_btn) {
    btn_x = (uint16_t)(x1 - 2);
    screen_text(s, btn_x, r.y, "+", fg, NO_COLOR, ATTR_BOLD);
    char action[48];
    snprintf(action, sizeof action, "split:%u", leaf->id);
    hit_add(&s->hits, btn_x, r.y, 1, 1, action);
    avail = (uint16_t)(avail - 3);
  }

  const char *title = pane_title(leaf->pane);
  if (title && *title && avail >= 3) {
    char buf[256];
    int len = snprintf(buf, sizeof buf, " %s ", title);
    if (len > (int)avail) {
      len = (int)avail;
      buf[len] = 0;
    }
    uint16_t tx = (uint16_t)(r.x + 1);
    if (CFG.title_align == ALIGN_CENTER)
      tx = (uint16_t)(r.x + 1 + (avail - len) / 2);
    else if (CFG.title_align == ALIGN_RIGHT)
      tx = (uint16_t)(r.x + 1 + avail - len);
    screen_text(s, tx, r.y, buf, focused ? TITLE_FOCUS : fg, NO_COLOR,
                focused ? ATTR_BOLD : 0);
  }
}

struct draw {
  app_t *a;
  screen_t *s;
};

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
  pane_compose(n->pane, d->s, n->content.x, n->content.y, n == cur(d->a)->focus);
}

static void draw_tab_strip(app_t *a, screen_t *s) {
  uint16_t x = (uint16_t)(CFG.gap * CFG.gap_aspect);
  for (size_t i = 0; i < a->ntabs && x < s->cols; i++) {
    tab_t *t = &a->tabs[i];
    char label[80];
    const char *nm = t->name[0] ? t->name : (t->purpose[0] ? t->purpose : "");
    if (nm[0]) snprintf(label, sizeof label, " %zu:%s ", i + 1, nm);
    else snprintf(label, sizeof label, " %zu ", i + 1);

    bool active = i == a->cur;
    uint16_t w = screen_text(s, x, CFG.gap, label,
                             active ? TITLE_FOCUS : FRAME_IDLE, NO_COLOR,
                             active ? ATTR_BOLD : 0);
    char action[48];
    snprintf(action, sizeof action, "tab:%u", t->id);
    hit_add(&s->hits, x, CFG.gap, w, 1, action);
    x = (uint16_t)(x + w);
  }
}

void app_compose(app_t *a, screen_t *s) {
  screen_clear(s); /* every frame starts blank: no ghosts in the gap ring */
  hit_reset(&s->hits);
  s->cursor_visible = false;
  a->painted = s;
  if (!a->ntabs || !cur(a)->root) return;
  layout(a);
  draw_tab_strip(a, s);
  struct draw d = {a, s};
  walk(cur(a)->root, draw_cb, &d);
}

/* ---- input -------------------------------------------------------------- */

static node_t *by_id(app_t *a, uint32_t id) {
  struct byid b = {id, NULL};
  walk(cur(a)->root, byid_cb, &b);
  return b.found;
}

static void do_action(app_t *a, const char *action, const input_event_t *ev) {
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
  } else if (strncmp(action, "close:", 6) == 0) {
    close_leaf(a, n);
  } else if (strncmp(action, "pane:", 5) == 0) {
    cur(a)->focus = n;
    /* translate to pane-local coordinates before forwarding */
    input_event_t local = *ev;
    local.mx = (uint16_t)(ev->mx - n->content.x);
    local.my = (uint16_t)(ev->my - n->content.y);
    pane_send_mouse(n->pane, &local);
  } else if (strncmp(action, "focus:", 6) == 0) {
    cur(a)->focus = n;
  }
}

static bool prefix_command(app_t *a, const input_event_t *ev) {
  switch (ev->key) {
    case GHOSTTY_KEY_Q: a->quit = true; return true;
    case GHOSTTY_KEY_D: a->detach = true; return true;
    case GHOSTTY_KEY_BACKSLASH: /* C-a \ or C-a | */
      split_focus(a, SPLIT_COLS);
      return true;
    case GHOSTTY_KEY_MINUS:
      split_focus(a, SPLIT_ROWS);
      return true;
    case GHOSTTY_KEY_X:
      if (cur(a)->focus) close_leaf(a, cur(a)->focus);
      return true;
    case GHOSTTY_KEY_O: focus_next(a); return true;
    case GHOSTTY_KEY_C: app_new_tab(a, ""); return true;
    case GHOSTTY_KEY_N: app_cycle_tab(a, 1); return true;
    case GHOSTTY_KEY_P: app_cycle_tab(a, -1); return true;
    case GHOSTTY_KEY_H: case GHOSTTY_KEY_ARROW_LEFT: focus_dir(a, -1, 0); return true;
    case GHOSTTY_KEY_L: case GHOSTTY_KEY_ARROW_RIGHT: focus_dir(a, 1, 0); return true;
    case GHOSTTY_KEY_K: case GHOSTTY_KEY_ARROW_UP: focus_dir(a, 0, -1); return true;
    case GHOSTTY_KEY_J: case GHOSTTY_KEY_ARROW_DOWN: focus_dir(a, 0, 1); return true;
    default:
      if (ev->key >= GHOSTTY_KEY_DIGIT_1 && ev->key <= GHOSTTY_KEY_DIGIT_9) {
        app_select_tab(a, (size_t)(ev->key - GHOSTTY_KEY_DIGIT_1));
        return true;
      }
      return false;
  }
}

void app_event(app_t *a, const input_event_t *ev) {
  if (!cur(a)->focus) return;

  if (ev->kind == EV_KEY && ev->action != KEY_RELEASE) {
    bool ctrl_a = (ev->mods & MOD_CTRL) && ev->unshifted == 'a';
    if (a->prefix) {
      a->prefix = false;
      if (ctrl_a) { pane_send_key(cur(a)->focus->pane, ev); return; }
      prefix_command(a, ev); /* unbound: swallowed */
      return;
    }
    if (ctrl_a) {
      a->prefix = true;
      return;
    }
  }

  switch (ev->kind) {
    case EV_KEY:
      pane_send_key(cur(a)->focus->pane, ev);
      break;
    case EV_MOUSE: {
      /* Mouse routing is a hit-list lookup, never a re-derivation of geometry:
       * the list was filled by the pass that painted what the user clicked. */
      if (!a->painted) break;
      const char *action = hit_test(&a->painted->hits, ev->mx, ev->my);
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
