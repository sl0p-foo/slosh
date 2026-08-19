/* Layout, tree edits, resizing, equalising, rotating, focus. Split from app.c. */
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

/* ---- layout: a pure function of the tree and the rect ------------------- */

/* Every leaf under a node, in tree order. */
size_t collect_leaves(node_t *n, node_t **out, size_t cap, size_t k) {
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

size_t collect_minimized(node_t *n, node_t **out, size_t cap, size_t k) {
  if (!n || k >= cap) return k;
  if (n->kind == NODE_LEAF) {
    if (n->minimized) out[k++] = n;
    return k;
  }
  for (size_t i = 0; i < n->nkids; i++)
    k = collect_minimized(n->kids[i], out, cap, k);
  return k;
}

size_t count_leaves(node_t *n) {
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

node_t *first_leaf_of(node_t *n) {
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

node_t *pane_by_id(app_t *a, uint32_t id); /* defined below */
size_t tab_of(app_t *a, node_t *n);

void layout(app_t *a) {
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

void split_node(app_t *a, node_t *leaf, split_dir_t dir, bool before) {
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
bool split_fits(node_t *leaf, split_dir_t dir) {
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

split_dir_t side_dir(char side) {
  return (side == 'l' || side == 'r') ? SPLIT_COLS : SPLIT_ROWS;
}

void split_focus(app_t *a, split_dir_t dir) {
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
void split_focus_ui(app_t *a, split_dir_t dir) {
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
void split_focus_auto(app_t *a) {
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

void close_leaf(app_t *a, node_t *leaf) {
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

tab_t *tab_by_id(app_t *a, uint32_t id) {
  for (size_t i = 0; i < a->ntabs; i++)
    if (a->tabs[i].id == id) return &a->tabs[i];
  return NULL;
}

size_t tab_index(app_t *a, uint32_t id) {
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
void move_tab(app_t *a, size_t from, size_t to) {
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
node_t *pane_by_id(app_t *a, uint32_t id) {
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
void transfer_weight(node_t *from, node_t *to, int amount) {
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
void resize_focus(app_t *a, int dx, int dy) {
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
void rotate_layout_ui(app_t *a) {
  bool one_pane = cur(a)->root && cur(a)->root->kind == NODE_LEAF;
  if (!app_rotate_layout(a))
    app_toast(a, one_pane ? "nothing to turn" : "no room to turn it");
}

/* Reordering is a swap of two leaves, in place: their positions, weights and
 * parents trade, and everything else about them is untouched. */
bool swap_panes(app_t *a, uint32_t id_a, uint32_t id_b) {
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

void focus_dir(app_t *a, int dx, int dy) {
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

void focus_next(app_t *a) {
  struct nextsearch s = {cur(a)->focus, NULL, NULL, NULL, false};
  walk(cur(a)->root, next_cb, &s);
  cur(a)->focus = s.next ? s.next : s.first;
}
