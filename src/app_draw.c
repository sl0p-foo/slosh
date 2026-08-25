/* Drawing, the shader passes, and kitty graphics re-emission. Split from app.c. */
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

/* ---- drawing ------------------------------------------------------------ */

/* ---- compact: shared borders -------------------------------------------
 *
 * Compact packs panes flush against 1-cell divider lines and rings the tab
 * with one outer frame, instead of giving every pane its own border across a
 * gap. The lines are drawn as *strokes*: each write says which of the cell's
 * four edges it needs (up/down/left/right), reads what is already there, and
 * writes the union — so a divider running into the outer frame makes a ├ and
 * two dividers crossing make a ┼ without anyone computing junctions. A cell
 * holding anything that is not a line (a title, a button) is left alone, so
 * a re-stroke — the focused pane's ring in the frame colour — can run over a
 * finished row and recolour exactly the lines. */

enum { BX_U = 1, BX_D = 2, BX_L = 4, BX_R = 8 };

/* The glyph for a set of edge bits. Corners — exactly one arm each way —
 * follow `rounded`, the same choice the classic frame makes; a tee or a
 * cross has no rounded form. */
static const char *box_glyph(uint8_t bits) {
  switch (bits) {
  case BX_L | BX_R: return "\u2500";                          /* ─ */
  case BX_U | BX_D: return "\u2502";                          /* │ */
  case BX_D | BX_R: return CFG.rounded ? "\u256d" : "\u250c"; /* ┌ */
  case BX_D | BX_L: return CFG.rounded ? "\u256e" : "\u2510"; /* ┐ */
  case BX_U | BX_R: return CFG.rounded ? "\u2570" : "\u2514"; /* └ */
  case BX_U | BX_L: return CFG.rounded ? "\u256f" : "\u2518"; /* ┘ */
  case BX_U | BX_D | BX_R: return "\u251c";                   /* ├ */
  case BX_U | BX_D | BX_L: return "\u2524";                   /* ┤ */
  case BX_D | BX_L | BX_R: return "\u252c";                   /* ┬ */
  case BX_U | BX_L | BX_R: return "\u2534";                   /* ┴ */
  case BX_U | BX_D | BX_L | BX_R: return "\u253c";            /* ┼ */
  case BX_U: return "\u2575";                                 /* ╵ */
  case BX_D: return "\u2577";                                 /* ╷ */
  case BX_L: return "\u2574";                                 /* ╴ */
  case BX_R: return "\u2576";                                 /* ╶ */
  default: return " ";
  }
}

/* And back: the edge bits of what a cell already holds, or 0 for anything
 * that is not one of our lines. The dashed variants count — a drop-target's
 * dashes are still the boundary they replaced — and both corner styles do,
 * so `rounded` cannot confuse the union. */
static uint8_t box_bits_of(const cell_t *c) {
  if (!c || c->len != 3) return 0;
  static const struct {
    const char *g;
    uint8_t bits;
  } tab[] = {
      {"\u2500", BX_L | BX_R},
      {"\u2502", BX_U | BX_D},
      {"\u2504", BX_L | BX_R},
      {"\u2506", BX_U | BX_D},
      {"\u250c", BX_D | BX_R},
      {"\u256d", BX_D | BX_R},
      {"\u2510", BX_D | BX_L},
      {"\u256e", BX_D | BX_L},
      {"\u2514", BX_U | BX_R},
      {"\u2570", BX_U | BX_R},
      {"\u2518", BX_U | BX_L},
      {"\u256f", BX_U | BX_L},
      {"\u251c", BX_U | BX_D | BX_R},
      {"\u2524", BX_U | BX_D | BX_L},
      {"\u252c", BX_D | BX_L | BX_R},
      {"\u2534", BX_U | BX_L | BX_R},
      {"\u253c", BX_U | BX_D | BX_L | BX_R},
      {"\u2575", BX_U},
      {"\u2577", BX_D},
      {"\u2574", BX_L},
      {"\u2576", BX_R},
  };
  for (size_t i = 0; i < sizeof tab / sizeof *tab; i++)
    if (memcmp(c->text, tab[i].g, 3) == 0) return tab[i].bits;
  return 0;
}

/* One stroke into one cell: union with whatever line is there, and never
 * clobber anything that is not a line — which is what lets a focus re-stroke
 * run over a row that already carries a title. `create` false additionally
 * refuses blank cells: a junction extension and a focus ring only ever join
 * or recolour lines this pass drew, and must not leave stray stumps where
 * there is no line to meet — the rows the minimised bar owns, say. */
static void stroke(screen_t *s, uint16_t x, uint16_t y, uint8_t bits,
                   color_t fg, uint16_t attrs, bool create) {
  cell_t *c = screen_at(s, x, y);
  if (!c) return;
  uint8_t have = box_bits_of(c);
  if (!have && !(create && c->len == 1 && c->text[0] == ' ')) return;
  screen_text(s, x, y, box_glyph((uint8_t)(have | bits)), fg, NO_COLOR, attrs);
}

/* The ring one cell outside `r`: the four lines a compact pane sits inside.
 * Drawn (`create`) for the tab area it is the outer frame; re-stroked for
 * the focused pane it *recolours* its stretch of the shared lines — junction
 * glyphs included — and adds no arms of its own: any bit the ring would add
 * is an arm pointing at a line that is not there, which is exactly the state
 * (a pane sitting on the minimised bar's rows) where nothing should be
 * drawn. */
static void stroke_ring(screen_t *s, rect_t r, color_t fg, uint16_t attrs,
                        bool create) {
  if (!r.x || !r.y || !r.w || !r.h) return;
  uint16_t x0 = (uint16_t)(r.x - 1), x1 = (uint16_t)(r.x + r.w);
  uint16_t y0 = (uint16_t)(r.y - 1), y1 = (uint16_t)(r.y + r.h);
  for (uint16_t x = x0; x <= x1; x++) {
    uint8_t h =
        create ? (uint8_t)((x > x0 ? BX_L : 0) | (x < x1 ? BX_R : 0)) : 0;
    stroke(s, x, y0, h, fg, attrs, create);
    stroke(s, x, y1, h, fg, attrs, create);
  }
  for (uint16_t y = y0; y <= y1; y++) {
    uint8_t v =
        create ? (uint8_t)((y > y0 ? BX_U : 0) | (y < y1 ? BX_D : 0)) : 0;
    stroke(s, x0, y, v, fg, attrs, create);
    stroke(s, x1, y, v, fg, attrs, create);
  }
}

/* Is this pane drawn compact? The mode is global but the treatment is not:
 * a float is an overlay and keeps the classic frame that lifts it off the
 * page, and a flattened or zoomed tab is showing one pane on its own —
 * nothing is packed against anything, so the pane keeps its own edges. */
static bool compact_pane(app_t *a, const node_t *leaf) {
  return CFG.compact && !leaf->floating && !leaf->collapsed && !a->flattened &&
         a->ntabs && !cur(a)->zoom;
}

/* Does this side of the rect sit on the tab's outer frame? Interior sides
 * are shared dividers — resize handles their whole length — and only the
 * outer frame keeps the click-to-split border verbs. Geometric on purpose:
 * when the minimised bar eats the bottom rows, the panes above it touch no
 * line, and offering a split target on the bar's cells would be a lie. */
static bool edge_outer(app_t *a, rect_t r, char side) {
  rect_t ar = app_tab_area(a);
  switch (side) {
  case 'l': return r.x == ar.x;
  case 'r': return (uint16_t)(r.x + r.w) == (uint16_t)(ar.x + ar.w);
  case 't': return r.y == ar.y;
  default: return (uint16_t)(r.y + r.h) == (uint16_t)(ar.y + ar.h);
  }
}

/* A plain rule goes dashed, and anything else — a junction, a title, a blank
 * — stays: how a compact pane's stretch of the shared lines says "you could
 * drop here" without redrawing glyphs its neighbours also own. */
static void dash_rule(screen_t *s, uint16_t x, uint16_t y, color_t fg,
                      uint16_t attrs) {
  cell_t *c = screen_at(s, x, y);
  if (!c || c->len != 3) return;
  if (memcmp(c->text, "\u2500", 3) == 0)
    screen_text(s, x, y, "\u2504", fg, NO_COLOR, attrs);
  else if (memcmp(c->text, "\u2502", 3) == 0)
    screen_text(s, x, y, "\u2506", fg, NO_COLOR, attrs);
}

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
    uint16_t drawn = screen_text(
        s, left, y, buf, dead ? DEAD_C : (focused ? TITLE_FOCUS : TITLE_IDLE),
        NO_COLOR, dead ? ATTR_BOLD : 0);
    /* Claims its cells, the way the name on the top row does. Nothing clicks it
     * -- an epitaph is not a button -- but owning the cells is what keeps the
     * armed split guide from ruling a line through the one sentence saying how
     * this pane died. */
    if (drawn) {
      char action[48];
      snprintf(action, sizeof action, "panestatus:%u", leaf->id);
      hit_add(&s->hits, left, y, drawn, 1, action);
    }
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
 * it once those are known.
 *
 * A compact pane's edges are the shared lines one cell outside its rect, so
 * `cf` moves the handle out there; the span is the pane's own stretch of the
 * line, junction cells excluded by construction since they sit beyond it. */
static bool split_handle(const node_t *leaf, char side, bool cf, rect_t *out) {
  rect_t r = leaf->rect;
  if (cf ? (r.w < 4 || r.h < 3 || !r.x || !r.y) : (r.w < 4 || r.h < 4))
    return false;
  bool vert = side == 'l' || side == 'r';
  /* The span leaves the corners out: a corner is where two gaps cross and is a
   * resize target already. */
  uint16_t span =
      vert ? (uint16_t)(r.h - (cf ? 0 : 2)) : (uint16_t)(r.w - (cf ? 0 : 2));
  if (span < 3) return false;
  uint16_t len = split_handle_len(span);
  uint16_t off = (uint16_t)((span - len) / 2);
  if (vert) {
    uint16_t bx =
        side == 'l' ? (cf ? (uint16_t)(r.x - 1) : r.x)
                    : (cf ? (uint16_t)(r.x + r.w) : (uint16_t)(r.x + r.w - 1));
    *out = (rect_t){bx, (uint16_t)(r.y + (cf ? 0 : 1) + off), 1, len};
  } else {
    uint16_t by = cf ? (uint16_t)(r.y + r.h) : (uint16_t)(r.y + r.h - 1);
    *out = (rect_t){(uint16_t)(r.x + (cf ? 0 : 1) + off), by, len, 1};
  }
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

/* And what the rest of that side is called: the part that arms the guide and
 * does nothing when pressed. The top row's rim is the drag handle, so it keeps
 * the plain `title:` name it always had. */
static void split_rim_action(const node_t *leaf, char side, char *buf,
                             size_t cap) {
  if (side == 't')
    snprintf(buf, cap, "title:%u", leaf->id);
  else
    snprintf(buf, cap, "brim:%u:%c", leaf->id, side);
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
  bool cf = compact_pane(a, leaf);
  if (cf ? (r.w < 4 || r.h < 3) : (r.w < 4 || r.h < 4)) return;
  uint16_t x1 = (uint16_t)(r.x + r.w - 1), y1 = (uint16_t)(r.y + r.h - 1);
  color_t hi = GUIDE;

  /* Stage one: the armed edge, on the cells the edge actually owns.
   *
   * A pane's name, its buttons and its scroll indicator all live on the top
   * border, and the first version ruled straight through them: the row went
   * unreadable exactly while the pointer was on it, and the handle looked as
   * though it had been dropped at random, because the title it sits beside had
   * been painted over. Asking the hit list which cells belong to this edge is
   * the same question the handle already asks two lines down, and it costs one
   * lookup per cell of one edge.
   *
   * The dead row's own buttons sit on the bottom border for the same reason, so
   * this is not a top-row special case. */
  char rim[48], act[48];
  split_rim_action(leaf, side, rim, sizeof rim);
  split_handle_action(leaf, side, act, sizeof act);
  if (side == 'l' || side == 'r') {
    uint16_t bx = side == 'l' ? (cf ? (uint16_t)(r.x - 1) : r.x)
                              : (cf ? (uint16_t)(r.x + r.w) : x1);
    uint16_t ylo = (uint16_t)(r.y + (cf ? 0 : 1));
    uint16_t yhi = cf ? (uint16_t)(r.y + r.h) : y1;
    for (uint16_t y = ylo; y < yhi; y++) {
      const char *own = hit_test(&s->hits, bx, y);
      if (own && (strcmp(own, rim) == 0 || strcmp(own, act) == 0))
        screen_text(s, bx, y, "\u2503", hi, NO_COLOR, ATTR_BOLD);
    }
  } else {
    uint16_t by = side == 't' ? (cf ? (uint16_t)(r.y - 1) : r.y)
                              : (cf ? (uint16_t)(r.y + r.h) : y1);
    uint16_t xlo = (uint16_t)(r.x + (cf ? 0 : 1));
    uint16_t xhi = cf ? (uint16_t)(r.x + r.w) : x1;
    for (uint16_t x = xlo; x < xhi; x++) {
      const char *own = hit_test(&s->hits, x, by);
      if (own && (strcmp(own, rim) == 0 || strcmp(own, act) == 0))
        screen_text(s, x, by, "\u2501", hi, NO_COLOR, ATTR_BOLD);
    }
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

/* The drop zones: while a pane is being carried, the pane under the pointer
 * subdivides into a centre — today's swap, keeping the biggest target — and
 * four edge bands that mean "insert me beside this pane, on this side",
 * which is what turns dragging into re-layout. Bands are rects registered in
 * the hit list as they are painted, so the promise on screen and the drop on
 * release are the same rects; the corners go to the upright bands because
 * they are registered last, and the centre needs no entry at all — a
 * pointer that lands in no band falls through to the pane's own hit, which
 * is the swap it always was.
 *
 * Zones materialise only under the pointer: four bands of chrome on every
 * candidate during every drag would be noise, and the drop highlight has
 * always been a fact about the pane you are on. A side whose insert would
 * not fit (the same floor split_fits holds a border click to) simply never
 * registers, so the preview cannot promise what the drop cannot deliver.
 * The hovered band fills the half of the pane the drop would hand over —
 * where the pane will *be*, not where the button is. */
static void draw_drop_zones(app_t *a, screen_t *s, node_t *n) {
  if (a->drag.kind != DRAG_TITLE || !a->drag.moved) return;
  if (n->id == a->drag.src || n->floating) return;
  if (!a->ptr_valid) return;
  rect_t r = n->rect;
  if (r.w < 3 || r.h < 3) return;
  if (a->ptr_x < r.x || a->ptr_x >= r.x + r.w || a->ptr_y < r.y ||
      a->ptr_y >= r.y + r.h)
    return;

  /* A third of each dimension per band, so the centre is the remaining
   * third: big enough to hit without aiming, small enough that the bands —
   * the half of this feature you steer by — own most of the pane. It began
   * as quarters, and the first map of the result showed the swap owning
   * half of each axis: a target that big reads as dead space when you are
   * mid-drag hunting for a band. */
  uint16_t bw = r.w / 3 ? (uint16_t)(r.w / 3) : 1;
  uint16_t bh = r.h / 3 ? (uint16_t)(r.h / 3) : 1;

  /* Whether a side is offered. A drop beside a *sibling along that axis* is
   * a reorder — the dragged pane leaves the split it would rejoin, so the
   * child count never changes and there is nothing to fit; refusing it was
   * refusing the most ordinary drag there is, rearranging a row. Everything
   * else is a real insert and answers to the same floor a border click
   * does. (Still conservative across splits: a drop whose *source* leaving
   * would have made the room is not offered, because the offer would need
   * the speculative layout to prove.) */
  node_t *src = pane_by_id(a, a->drag.src);
  bool sib = src && src->parent && src->parent == n->parent;
  bool rows_ok =
      (sib && n->parent->dir == SPLIT_ROWS) || split_fits(n, SPLIT_ROWS);
  bool cols_ok =
      (sib && n->parent->dir == SPLIT_COLS) || split_fits(n, SPLIT_COLS);

  char action[48];
  /* Horizontal bands first, upright bands after: the corners resolve to a
   * column split, which is the axis a cell's shape makes roomier. */
  if (rows_ok) {
    snprintf(action, sizeof action, "drop:%u:t", n->id);
    hit_add(&s->hits, r.x, r.y, r.w, bh, action);
    snprintf(action, sizeof action, "drop:%u:b", n->id);
    hit_add(&s->hits, r.x, (uint16_t)(r.y + r.h - bh), r.w, bh, action);
  }
  if (cols_ok) {
    snprintf(action, sizeof action, "drop:%u:l", n->id);
    hit_add(&s->hits, r.x, r.y, bw, r.h, action);
    snprintf(action, sizeof action, "drop:%u:r", n->id);
    hit_add(&s->hits, (uint16_t)(r.x + r.w - bw), r.y, bw, r.h, action);
  }

  /* The fill, read back off the hits just registered so it cannot disagree
   * with what a release here would do. */
  const char *over = hit_test(&s->hits, a->ptr_x, a->ptr_y);
  char want[16];
  snprintf(want, sizeof want, "drop:%u:", n->id);
  if (!over || strncmp(over, want, strlen(want)) != 0) return;
  char side = over[strlen(want)];
  rect_t half = r;
  switch (side) {
  case 'l': half.w = (uint16_t)(r.w / 2); break;
  case 'r':
    half.w = (uint16_t)(r.w / 2);
    half.x = (uint16_t)(r.x + r.w - half.w);
    break;
  case 't': half.h = (uint16_t)(r.h / 2); break;
  default:
    half.h = (uint16_t)(r.h / 2);
    half.y = (uint16_t)(r.y + r.h - half.h);
    break;
  }
  shader_t fill;
  if (!shader_make(&fill, "tint", DROP_C, 110)) return;
  shade_ctx_t base = {
      .now_ms = now_ms_(),
      .default_fg = CFG.default_fg,
      .default_bg = CFG.default_bg,
  };
  shade_apply(s, &fill, 1, half, NULL, &base);
}

/* The float's answer to the split guide: hovering its border shows, in the
 * resize colour, which edges a grab there would move — one lit edge for a
 * side, two meeting at the pointer for a corner — and the pair stays lit
 * while the drag holds them. Without this the border was a working handle
 * that never said so, which is a control you have to already know about.
 *
 * The edges are read from float_edges_at, the same derivation the press and
 * the hint use, and each cell is painted only if the hit list says the
 * border still owns it — so the buttons and the epitaph on the bottom row
 * stay legible, the same question the split guide asks. Armed on the same
 * dwell, for the same reason: a pointer crossing a float on its way
 * somewhere else should not make the frame flash. */
static void draw_float_guide(app_t *a, screen_t *s, node_t *leaf) {
  if (!leaf->floating || leaf->collapsed || leaf->hidden) return;
  rect_t r = leaf->rect;
  if (r.w < 3 || r.h < 3) return;

  uint8_t e = 0;
  bool active = a->drag.kind == DRAG_FLOAT_RESIZE && a->drag.src == leaf->id;
  if (active) {
    e = a->drag.fedges;
  } else if (a->drag.kind == DRAG_NONE && a->ptr_valid) {
    if (now_ms_() - a->ptr_still_since < CFG.hover_delay_ms) return;
    const char *action = hit_test(&s->hits, a->ptr_x, a->ptr_y);
    if (!action) return;
    char want[24];
    snprintf(want, sizeof want, "border:%u:", leaf->id);
    bool mine = strncmp(action, want, strlen(want)) == 0;
    if (!mine) {
      snprintf(want, sizeof want, "brim:%u:", leaf->id);
      mine = strncmp(action, want, strlen(want)) == 0;
    }
    if (!mine) return;
    e = float_edges_at(leaf, a->ptr_x, a->ptr_y);
  }
  if (!e) return;

  uint16_t x1 = (uint16_t)(r.x + r.w - 1), y1 = (uint16_t)(r.y + r.h - 1);
  uint16_t attrs = ATTR_BOLD;

  /* A cell is painted only if the border owns it: brim or handle, either
   * name is the edge. */
  char rim[24], handle[24];
  for (int side = 0; side < 3; side++) {
    char c = "lrb"[side];
    uint8_t bit = c == 'l' ? FEDGE_L : c == 'r' ? FEDGE_R : FEDGE_B;
    if (!(e & bit)) continue;
    snprintf(rim, sizeof rim, "brim:%u:%c", leaf->id, c);
    snprintf(handle, sizeof handle, "border:%u:%c", leaf->id, c);
    if (c == 'b') {
      for (uint16_t x = r.x; x <= x1; x++) {
        const char *own = hit_test(&s->hits, x, y1);
        if (own && (strcmp(own, rim) == 0 || strcmp(own, handle) == 0))
          screen_text(s, x, y1, "\u2501", RESIZE_C, NO_COLOR, attrs);
      }
    } else {
      uint16_t bx = c == 'l' ? r.x : x1;
      for (uint16_t y = (uint16_t)(r.y + 1); y < y1; y++) {
        const char *own = hit_test(&s->hits, bx, y);
        if (own && (strcmp(own, rim) == 0 || strcmp(own, handle) == 0))
          screen_text(s, bx, y, "\u2503", RESIZE_C, NO_COLOR, attrs);
      }
    }
  }

  /* The double arrow at the grab point, hover only — mid-drag the pointer
   * has left the edge, and the lit edges following the hand are the
   * feedback. A corner gets none: two edges meeting at the pointer already
   * say "both ways", and no single glyph does. */
  bool corner = (e & (FEDGE_L | FEDGE_R)) && (e & (FEDGE_T | FEDGE_B));
  if (!active && !corner)
    screen_text(s, a->ptr_x, a->ptr_y,
                (e & (FEDGE_L | FEDGE_R)) ? "\u21d4" : "\u21d5", RESIZE_C,
                NO_COLOR, ATTR_BOLD);
}

static void draw_frame(app_t *a, screen_t *s, node_t *leaf) {
  rect_t r = leaf->rect;
  /* Compact wears no frame of its own: the lines were drawn by
   * draw_compact_lines and this function only dresses them — the title, the
   * buttons and the hits go on the shared line *above* the rect, which is
   * why a compact pane needs a row and a column to its left to exist. */
  bool cf = compact_pane(a, leaf);
  if (cf ? (r.w < 3 || !r.h || !r.x || !r.y) : (r.w < 3 || r.h < 3)) return;
  uint16_t ty = cf ? (uint16_t)(r.y - 1) : r.y; /* the row the title rides */
  bool top_outer = !cf || edge_outer(a, r, 't');
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
  bool drag_target = a->drag.kind == DRAG_TITLE && a->drag.moved &&
                     a->drag.src != leaf->id && !leaf->floating;
  const char *hbar = drag_target ? "\u2504" : "\u2500";
  const char *vbar = drag_target ? "\u2506" : "\u2502";

  const char *tl = CFG.rounded ? "╭" : "┌", *tr = CFG.rounded ? "╮" : "┐";
  const char *bl = CFG.rounded ? "╰" : "└", *br = CFG.rounded ? "╯" : "┘";

  uint16_t x1 = (uint16_t)(r.x + r.w - 1), y1 = (uint16_t)(r.y + r.h - 1);
  if (!cf) {
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

    /* The padding ring between the border and the contents, painted blank
     * rather than left alone. A tiled pane sits on a cleared screen, so
     * leaving it unpainted used to look the same by accident — but a float
     * sits on whatever was composited under it, and every unpainted cell lets
     * that show through. Blank-with-no-colour is exactly what screen_clear
     * leaves, so a tiled pane looks as it always did, and with no padding
     * configured the ring is empty and this writes nothing. */
    rect_t c = leaf->content;
    for (uint16_t y = (uint16_t)(r.y + 1); y < y1; y++)
      for (uint16_t x = (uint16_t)(r.x + 1); x < x1; x++) {
        if (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h) {
          x = (uint16_t)(c.x + c.w - 1); /* jump past the contents */
          continue;
        }
        screen_text(s, x, y, " ", NO_COLOR, NO_COLOR, 0);
      }
  } else if (drag_target || drop_target) {
    /* The dashed drop-candidate border, said on shared lines: each cell of
     * this pane's ring that is still a plain rule goes dashed (in DROP_C for
     * the pane under the pointer). Junctions are left alone — they belong to
     * the neighbours too, and a dashed tee is not a glyph. */
    uint16_t rx0 = (uint16_t)(r.x - 1), rx1 = (uint16_t)(r.x + r.w);
    uint16_t ry0 = ty, ry1 = (uint16_t)(r.y + r.h);
    for (uint16_t x = rx0; x <= rx1; x++) {
      dash_rule(s, x, ry0, fg, attrs);
      dash_rule(s, x, ry1, fg, attrs);
    }
    for (uint16_t y = ry0; y <= ry1; y++) {
      dash_rule(s, rx0, y, fg, attrs);
      dash_rule(s, rx1, y, fg, attrs);
    }
  }

  /* A pane that rang, marked just inside its corner: the same place on every
   * pane, whatever its title is doing. */
  if (CFG.bell_indicator && pane_bell(leaf->pane) && r.w > 4)
    screen_text(s, (uint16_t)(r.x + 1), ty, CFG.bell_mark, BELL_C, NO_COLOR,
                ATTR_BOLD);

  /* The frame's top row is the drag handle, all of it. Its *split* is a handle
   * on the same row, but it is placed at the very end of this function rather
   * than here: the title, the buttons and the scroll indicator all anchor
   * wherever the config puts them, a centred title lands exactly where a
   * centred handle would want to be, and this row's rule is that the title
   * wins. So the handle is put where they are not, once they are all placed.
   *
   * Not when the row is a shared divider (compact, interior top edge): that
   * line is the boundary between this pane and the one above, and its free
   * cells stay the resize handle they are. The pane is still dragged — by
   * its name, which registers its own hit below. */
  if (top_outer) {
    char action[48];
    snprintf(action, sizeof action, "title:%u", leaf->id);
    hit_add(&s->hits, r.x, ty, r.w, 1, action);
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
      bool hot = ptr_on(a, px, ty, bw, 1);
      screen_text(s, px, ty, cell, hot ? PANE_BTN_HOVER : PANE_BTN, NO_COLOR,
                  hot ? ATTR_BOLD : 0);
      char action[48];
      snprintf(action, sizeof action, "%s:%u", btns[i].verb, leaf->id);
      hit_add(&s->hits, px, ty, bw, 1, action);
      bx = (uint16_t)(px - 1);
      has_btn = true;
      btn_x = px;
      avail = (uint16_t)(avail > bw ? avail - bw : 0);
    }
    /* One blank between the rule and the first button, so the group is not
     * welded to the frame the way a title without its space would be. */
    if (has_btn && btn_x > (uint16_t)(r.x + 1)) {
      screen_text(s, (uint16_t)(btn_x - 1), ty, " ", PANE_BTN, NO_COLOR, 0);
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
  if (!cf) {
    char action[48];
    snprintf(action, sizeof action, "brim:%u:l", leaf->id);
    hit_add(&s->hits, r.x, (uint16_t)(r.y + 1), 1, (uint16_t)(r.h - 2), action);
    snprintf(action, sizeof action, "brim:%u:r", leaf->id);
    hit_add(&s->hits, x1, (uint16_t)(r.y + 1), 1, (uint16_t)(r.h - 2), action);
    snprintf(action, sizeof action, "brim:%u:b", leaf->id);
    hit_add(&s->hits, r.x, y1, r.w, 1, action);
    for (const char *side = "lrb"; *side; side++) {
      rect_t h;
      if (!split_handle(leaf, *side, false, &h)) continue;
      split_handle_action(leaf, *side, action, sizeof action);
      hit_add(&s->hits, h.x, h.y, h.w, h.h, action);
    }
  } else {
    /* Only the outer frame keeps the border verbs. An interior side is a
     * shared divider: its whole length is already the resize handle the gap
     * used to be, and a one-cell line cannot honestly hold two verbs — the
     * keyboard still splits anything. */
    char action[48];
    uint16_t xl = (uint16_t)(r.x - 1), xr = (uint16_t)(r.x + r.w);
    uint16_t yb = (uint16_t)(r.y + r.h);
    if (edge_outer(a, r, 'l')) {
      snprintf(action, sizeof action, "brim:%u:l", leaf->id);
      hit_add(&s->hits, xl, r.y, 1, r.h, action);
    }
    if (edge_outer(a, r, 'r')) {
      snprintf(action, sizeof action, "brim:%u:r", leaf->id);
      hit_add(&s->hits, xr, r.y, 1, r.h, action);
    }
    if (edge_outer(a, r, 'b')) {
      snprintf(action, sizeof action, "brim:%u:b", leaf->id);
      hit_add(&s->hits, r.x, yb, r.w, 1, action);
    }
    for (const char *side = "lrb"; *side; side++) {
      rect_t h;
      if (!edge_outer(a, r, *side)) continue;
      if (!split_handle(leaf, *side, true, &h)) continue;
      split_handle_action(leaf, *side, action, sizeof action);
      hit_add(&s->hits, h.x, h.y, h.w, h.h, action);
    }
  }

  /* A compact pane's status rides its bottom content row and must go over
   * the composed contents, so draw_cb calls it after the pane does. */
  if (!cf) draw_pane_status(a, s, leaf, fg, focused);

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
          screen_text(s, ix, ty, ind, SCROLL_FG, SCROLL_BG, ATTR_BOLD);
      char action[48];
      snprintf(action, sizeof action, "scrollbottom:%u", leaf->id);
      hit_add(&s->hits, ix, ty, drawn, 1, action);
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
        s, tx, ty, buf,
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
      hit_add(&s->hits, tx, ty, drawn, 1, action);
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
  if (top_outer) {
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

    /* As near the middle of the row as the row allows.
     *
     * Centred in the widest *run* was the first rule and it looked arbitrary,
     * because it is: with a centred title the row leaves two runs of nearly the
     * same size, one of them a cell longer for reasons no one can see, and the
     * handle floated off into the middle of whichever won. Aiming at the row's
     * middle and sliding into the run instead means the handle hugs whatever is
     * in the way -- so it sits against the title, where the eye already is,
     * rather than halfway to the corner. With nothing in the way it is simply
     * centred, which is the same rule the other three sides follow. */
    uint16_t mid = (uint16_t)(r.x + r.w / 2);
    uint16_t cap = split_handle_len((uint16_t)(r.w - 2));
    bool found = false;
    uint16_t best_x = 0, best_w = 0, best_dist = 0;
    for (size_t i = 0; i < nruns; i++) {
      if (runs[i].hi < runs[i].lo) continue;
      uint16_t len = (uint16_t)(runs[i].hi - runs[i].lo + 1);
      if (len < 3) continue;
      uint16_t want = cap > len ? len : cap;
      uint16_t last = (uint16_t)(runs[i].hi - want + 1);
      uint16_t start = mid > want / 2 ? (uint16_t)(mid - want / 2) : runs[i].lo;
      if (start < runs[i].lo) start = runs[i].lo;
      if (start > last) start = last;
      uint16_t c = (uint16_t)(start + want / 2);
      uint16_t dist = (uint16_t)(c > mid ? c - mid : mid - c);
      /* Nearest wins; a tie goes to the longer handle, which can only happen
       * when one run is too short to hold a full one. */
      if (!found || dist < best_dist || (dist == best_dist && want > best_w)) {
        found = true;
        best_x = start;
        best_w = want;
        best_dist = dist;
      }
    }
    if (found) {
      char action[48];
      split_handle_action(leaf, 't', action, sizeof action);
      hit_add(&s->hits, best_x, ty, best_w, 1, action);
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
    /* drop_hover is the swap's promise — the whole pane trades places. A
     * pointer in one of its drop *zones* promises something narrower, and
     * the zone's own fill says it; the pane stays an ordinary candidate so
     * the two promises cannot show at once. */
    if (n->id == a->drag.target && !a->drag.drop_side) return PSTATE_DROP_HOVER;
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
  /* Above `unfocused`, deliberately: a float is never dimmed by
   * dim_unfocused. The thing on top of the stack reading at full strength is
   * what keeps it lifted off the page — the same reason `dragging` keeps no
   * default — and the frame colour still tells focus. Ships no chain; the
   * shadow does the telling apart. */
  if (n->floating) return PSTATE_FLOATING;
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

/* Scrollback as the viewport sees it, for content and chrome passes alike, so
 * an expression can say "there is more this way": lines hidden above the top
 * edge, and below the bottom. `total` from pane_scroll_pos is already the
 * scrollable overhang (history minus the viewport), so the split needs no
 * geometry of its own. */
static void ctx_scroll(shade_ctx_t *base, const node_t *n) {
  if (!n->pane) return;
  uint32_t off = 0, total = 0;
  pane_scroll_pos(n->pane, &off, &total);
  base->above = off;
  base->below = total > off ? total - off : 0;
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
  ctx_scroll(&base, n);
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
  ctx_scroll(&base, n);
  note_animation(a, chain, nc);
  shade_apply(s, chain, nc, r, hole, &base);
}

/* Drawn after the panes, so the corner's hit is registered after the two gap
 * hits it sits on and wins those cells: the cross is one target, not the
 * overlap of two. */
void draw_corners(app_t *a, screen_t *s) {
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

void draw_node(app_t *a, screen_t *s, node_t *n);

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
    /* Nothing has ever composed into this content rect, so paint it blank
     * first — for the same reason draw_frame paints the padding ring: a
     * floating suspended pane must not show what is under it. */
    for (uint16_t y = n->content.y; y < n->content.y + n->content.h; y++)
      for (uint16_t x = n->content.x; x < n->content.x + n->content.w; x++)
        screen_text(d->s, x, y, " ", NO_COLOR, NO_COLOR, 0);
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
  /* A compact pane's chrome is the ring of shared line it sits inside — one
   * cell out on every side — rather than the rect's own edge; the state
   * passes (a bell's flash, a theme's chrome chain) land there instead. The
   * shared cells belong to the neighbours too, so a chain that recolours an
   * *unfocused* frame will also touch the focused pane's stretch of a shared
   * line: two owners is what sharing means. */
  rect_t chrome_r = n->rect;
  if (compact_pane(d->a, n) && chrome_r.x && chrome_r.y) {
    chrome_r.x--;
    chrome_r.y--;
    chrome_r.w = (uint16_t)(chrome_r.w + 2);
    chrome_r.h = (uint16_t)(chrome_r.h + 2);
  }
  shade_chrome(d->a, d->s, n, chrome_r, &n->content);
  /* Over the contents and under the guides: the status row of a compact pane
   * overlays its last content row — there is no bottom border to carry it —
   * so it has to go on after the pane composed and shaded. */
  if (compact_pane(d->a, n)) {
    bool focused = n == cur(d->a)->focus;
    draw_pane_status(d->a, d->s, n, focused ? FRAME_FOCUS : FRAME_IDLE,
                     focused);
  }
  draw_split_guide(d->a, d->s, n);
  draw_float_guide(d->a, d->s, n);
  draw_drop_zones(d->a, d->s, n);
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

void draw_tab_strip(app_t *a, screen_t *s) {
  /* A pane is being carried: the strip is a row of destinations for as long as
   * that is true. Worked out once rather than per tab. */
  bool dragging_pane = a->drag.kind == DRAG_TITLE && a->drag.moved;
  uint16_t x = CFG.status_pad;
  uint16_t y = CFG.compact ? 0 : CFG.gap; /* compact spends no row on air */

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
void find_corners(app_t *a) {
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
/* A hint cell is painted only if the boundary still owns it. In classic
 * layouts a gap holds nothing else, so this refuses nothing; on a compact
 * divider the pane below has its title and buttons on the same line, and
 * ruling dots through a name would be the top-row mistake all over again. */
static bool edge_owns(screen_t *s, uint16_t x, uint16_t y) {
  const char *o = hit_test(&s->hits, x, y);
  return o && strncmp(o, "edge:", 5) == 0;
}

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
          if (edge_owns(s, x, y))
            screen_text(s, x, y, "\u250a", RESIZE_C, NO_COLOR, attrs0);
      } else {
        uint16_t y = (uint16_t)(gapr.y + gapr.h / 2);
        for (uint16_t x = gapr.x; x < gapr.x + gapr.w; x++)
          if (edge_owns(s, x, y))
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
      if (edge_owns(s, x, y))
        screen_text(s, x, y, active ? "\u2551" : "\u250a", RESIZE_C, NO_COLOR,
                    attrs);
    uint16_t ay = (uint16_t)(gapr.y + gapr.h / 2);
    if (edge_owns(s, x, ay))
      screen_text(s, x, ay, "\u21d4", RESIZE_C, NO_COLOR, ATTR_BOLD);
  } else {
    uint16_t y = (uint16_t)(gapr.y + gapr.h / 2);
    for (uint16_t x = gapr.x; x < gapr.x + gapr.w; x++)
      if (edge_owns(s, x, y))
        screen_text(s, x, y, active ? "\u2550" : "\u2508", RESIZE_C, NO_COLOR,
                    attrs);
    uint16_t ax = (uint16_t)(gapr.x + gapr.w / 2);
    if (edge_owns(s, ax, y))
      screen_text(s, ax, y, "\u21d5", RESIZE_C, NO_COLOR, ATTR_BOLD);
  }
}

void draw_node(app_t *a, screen_t *s, node_t *n) {
  if (n->collapsed) {
    draw_collapsed(a, s, n);
    return;
  }
  if (n->kind == NODE_LEAF) {
    /* A float is painted by draw_floats, after everything tiled, so its
     * cells and its hits go on top. A *collapsed* float took the branch
     * above: in a flattened tab it is a row like everyone else. */
    if (n->floating) return;
    struct draw d = {a, s};
    draw_cb(n, &d);
    return;
  }

  /* The gap between two children is the boundary you can drag. In classic
   * layouts it is drawn as nothing; compact draws the divider line through it
   * (draw_compact_lines). Either way it is a real target, derived from the
   * rects the children were just given rather than recomputed from the
   * config, and registered *before* the children so that a compact pane's
   * title and buttons — which live on this very line — win their own cells.
   * The hover hint is painted by draw_resize_hints once every pane has
   * claimed what is its. */
  for (size_t i = 0; i + 1 < n->nkids; i++) {
    rect_t gapr;
    if (!gap_rect(n, i, &gapr)) continue;
    char action[48];
    snprintf(action, sizeof action, "edge:%u:%zu", n->id, i);
    hit_add(&s->hits, gapr.x, gapr.y, gapr.w, gapr.h, action);
  }

  for (size_t i = 0; i < n->nkids; i++) draw_node(a, s, n->kids[i]);
}

/* The hover hints for every boundary, painted after the panes so the
 * ownership question each cell asks (`edge_owns`) is answered by the
 * finished hit list. */
void draw_resize_hints(app_t *a, screen_t *s) {
  if (!a->ntabs || !cur(a)->root) return;
  struct gapinfo g[64];
  size_t n = collect_gaps(cur(a)->root, g, 64, 0);
  for (size_t i = 0; i < n; i++)
    draw_resize_hint(a, s, g[i].sp, g[i].i, g[i].r);
}

/* The compact mode's lines, drawn before the panes so their titles land on
 * top: one frame ring round the tab area, a divider through every gap — each
 * extended one stroke into whatever perpendicular line it meets, which is
 * what makes the junctions — and the focused pane's ring re-stroked in the
 * frame colour, junction glyphs included, so focus reads off the lines the
 * way it always has. */
void draw_compact_lines(app_t *a, screen_t *s) {
  if (!CFG.compact || !a->ntabs || !cur(a)->root) return;
  if (a->flattened || cur(a)->zoom) return;
  rect_t ar = app_tab_area(a);
  if (!ar.x || !ar.y) return; /* no room for a ring: no lines to share */
  stroke_ring(s, ar, FRAME_IDLE, 0, true);

  /* The dividers' own cells first, their end-extensions after: an end only
   * ever joins a line that is already there — the ring, or a perpendicular
   * divider — which is what makes the junction glyphs, and what keeps a
   * divider that stops short (at the minimised bar's rows) from leaving a
   * stump in cells that hold no line. */
  struct gapinfo g[64];
  size_t n = collect_gaps(cur(a)->root, g, 64, 0);
  for (size_t i = 0; i < n; i++) {
    rect_t r = g[i].r;
    if (g[i].sp->dir == SPLIT_COLS)
      for (uint16_t y = r.y; y < r.y + r.h; y++)
        stroke(s, r.x, y, BX_U | BX_D, FRAME_IDLE, 0, true);
    else
      for (uint16_t x = r.x; x < r.x + r.w; x++)
        stroke(s, x, r.y, BX_L | BX_R, FRAME_IDLE, 0, true);
  }
  for (size_t i = 0; i < n; i++) {
    rect_t r = g[i].r;
    if (g[i].sp->dir == SPLIT_COLS) {
      if (r.y) stroke(s, r.x, (uint16_t)(r.y - 1), BX_D, FRAME_IDLE, 0, false);
      stroke(s, r.x, (uint16_t)(r.y + r.h), BX_U, FRAME_IDLE, 0, false);
    } else {
      if (r.x) stroke(s, (uint16_t)(r.x - 1), r.y, BX_R, FRAME_IDLE, 0, false);
      stroke(s, (uint16_t)(r.x + r.w), r.y, BX_L, FRAME_IDLE, 0, false);
    }
  }

  node_t *f = cur(a)->focus;
  if (f && f->kind == NODE_LEAF && !f->floating && !f->minimized &&
      !f->hidden && !f->collapsed && f->rect.x && f->rect.y)
    stroke_ring(s, f->rect, FRAME_FOCUS, 0, false);
}

/* The cell of shade a float casts on whatever it covers: `gap_aspect`
 * columns beside and one row below, offset so the light reads as coming from
 * the top left. The same dim pass the scrim is — a shadow is a colour pass
 * with everything in its rects — run before the float itself is painted, so
 * it falls on the composited cells underneath, lower floats included. The
 * two rects meet without overlapping, because a corner dimmed twice reads as
 * a stain rather than a shadow. */
static void cast_shadow(app_t *a, screen_t *s, rect_t r) {
  if (!CFG.float_shadow) return;
  shader_t dim;
  if (!shader_make(&dim, "dim", (color_t){0}, CFG.float_shadow)) return;
  shade_ctx_t base = {
      .now_ms = now_ms_(),
      .default_fg = CFG.default_fg,
      .default_bg = CFG.default_bg,
  };
  uint16_t off = CFG.gap_aspect ? CFG.gap_aspect : 2;
  shade_apply(s, &dim, 1,
              (rect_t){(uint16_t)(r.x + r.w), (uint16_t)(r.y + 1), off,
                       (uint16_t)(r.h - 1)},
              NULL, &base);
  shade_apply(s, &dim, 1,
              (rect_t){(uint16_t)(r.x + off), (uint16_t)(r.y + r.h), r.w, 1},
              NULL, &base);
}

/* The floating panes, over everything tiled and under the modals.
 *
 * Painted in ascending raise order, so the hit list — searched backwards —
 * resolves an overlap to the float you can see. The z-order and the click
 * order are the same list, which is the "one geometry" rule doing the work:
 * there is no routing code to disagree with the paint.
 *
 * Each float is drawn by the same draw_cb as a tiled pane: same frame, same
 * buttons, same shader passes. What differs is when, which is the whole
 * feature. */
void draw_floats(app_t *a, screen_t *s) {
  node_t *fl[64];
  size_t nf = collect_floating(cur(a)->root, fl, 64, 0);
  /* Insertion sort by raise stamp: n is small and the order is nearly
   * stable frame to frame. */
  for (size_t i = 1; i < nf; i++) {
    node_t *k = fl[i];
    size_t j = i;
    while (j > 0 && fl[j - 1]->raised > k->raised) {
      fl[j] = fl[j - 1];
      j--;
    }
    fl[j] = k;
  }
  for (size_t i = 0; i < nf; i++) {
    node_t *n = fl[i];
    /* A flattened tab listed it as a row, and a zoomed tab hid it: either
     * way this pass has nothing to add. */
    if (n->collapsed || n->hidden) continue;
    /* The cursor a tiled pane parked under this float must not glow through
     * it. Its own cursor is set by its own compose below, when focused. */
    if (s->cursor_visible && s->cursor_x >= n->rect.x &&
        s->cursor_x < n->rect.x + n->rect.w && s->cursor_y >= n->rect.y &&
        s->cursor_y < n->rect.y + n->rect.h)
      s->cursor_visible = false;
    cast_shadow(a, s, n->rect);
    struct draw d = {a, s};
    draw_cb(n, &d);
  }
}

/* ---- kitty graphics ------------------------------------------------------ */

struct gfx_ctx {
  app_t *a;
  node_t *leaf;
  /* The float rects drawn over this leaf's cells: every visible float for a
   * tiled pane, the higher-raised ones for a float. A placement is clipped
   * against them below, because kitty images are drawn by the client's
   * terminal *after* the cell diff — left alone, a picture in a tiled pane
   * paints straight over the float covering it. */
  const rect_t *occ;
  size_t nocc;
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

  /* The floats above this pane. The cell compositor gets occlusion free from
   * paint order; placements are sent after the diff and get it from this:
   * what one clean edge can express is cropped — the same source-rectangle
   * arithmetic as the pane-edge clipping, aimed at another clipper — and a
   * float in the *middle* of an image is a shape one placement cannot
   * express, so that placement is suppressed for the frame and returns when
   * the float moves. A corner overlap leaves an L, which is two rects, which
   * is the same refusal. */
  uint16_t col = g->col, row = g->row;
  uint32_t sx = g->sx, sy = g->sy;
  uint32_t xo = g->x_off, yo = g->y_off;
  for (size_t i = 0; i < c->nocc; i++) {
    int px0 = leaf->content.x + col, py0 = leaf->content.y + row;
    int px1 = px0 + cols, py1 = py0 + rows;
    int ox0 = c->occ[i].x, oy0 = c->occ[i].y;
    int ox1 = ox0 + c->occ[i].w, oy1 = oy0 + c->occ[i].h;
    if (ox1 <= px0 || ox0 >= px1 || oy1 <= py0 || oy0 >= py1) continue;
    bool spans_x = ox0 <= px0 && ox1 >= px1;
    bool spans_y = oy0 <= py0 && oy1 >= py1;
    if (spans_x && spans_y) return; /* fully covered */
    if (spans_x) {
      if (oy0 <= py0) { /* trimmed from the top: the source origin moves */
        uint16_t cut = (uint16_t)(oy1 - py0);
        if (g->req_rows) {
          uint32_t drop = rows ? (uint32_t)((uint64_t)sh * cut / rows) : 0;
          sy += drop;
          sh -= drop;
        } else {
          uint32_t px = (uint32_t)cut * g->cell_px_h;
          px = px > yo ? px - yo : 0;
          yo = 0;
          sy += px < sh ? px : sh;
          sh = px < sh ? sh - px : 0;
        }
        row = (uint16_t)(row + cut);
        rows = (uint16_t)(rows - cut);
      } else if (oy1 >= py1) { /* trimmed from the bottom, like the edge */
        uint16_t keep = (uint16_t)(oy0 - py0);
        if (g->req_rows) {
          sh = rows ? (uint32_t)((uint64_t)sh * keep / rows) : sh;
        } else {
          uint32_t px = (uint32_t)keep * g->cell_px_h;
          px = px > yo ? px - yo : 0;
          if (sh > px) sh = px;
        }
        rows = keep;
      } else {
        return; /* a strip across the middle */
      }
    } else if (spans_y) {
      if (ox0 <= px0) {
        uint16_t cut = (uint16_t)(ox1 - px0);
        if (g->req_cols) {
          uint32_t drop = cols ? (uint32_t)((uint64_t)sw * cut / cols) : 0;
          sx += drop;
          sw -= drop;
        } else {
          uint32_t px = (uint32_t)cut * g->cell_px_w;
          px = px > xo ? px - xo : 0;
          xo = 0;
          sx += px < sw ? px : sw;
          sw = px < sw ? sw - px : 0;
        }
        col = (uint16_t)(col + cut);
        cols = (uint16_t)(cols - cut);
      } else if (ox1 >= px1) {
        uint16_t keep = (uint16_t)(ox0 - px0);
        if (g->req_cols) {
          sw = cols ? (uint32_t)((uint64_t)sw * keep / cols) : sw;
        } else {
          uint32_t px = (uint32_t)keep * g->cell_px_w;
          px = px > xo ? px - xo : 0;
          if (sw > px) sw = px;
        }
        cols = keep;
      } else {
        return;
      }
    } else {
      return; /* a corner: the remainder is an L, not a rect */
    }
    if (!cols || !rows || !sw || !sh) return;
  }

  gfx_place(
      c->a->gfx,
      &(gfx_req_t){
          .pane = leaf->id,
          .src_id = g->image_id,
          .gen = g->generation,
          .place_id = g->place_id,
          .col = (uint16_t)(leaf->content.x + col),
          .row = (uint16_t)(leaf->content.y + row),
          .cols = cols,
          .rows = rows,
          /* The offsets survive the pane's own clipping untouched; an
       * occlusion trim from the left or top zeroes the one it consumed. */
          .x_off = xo,
          .y_off = yo,
          /* Clipped the same way the cell counts were, and zero when the program
       * never asked to scale. */
          .scale_cols = g->req_cols ? cols : 0,
          .scale_rows = g->req_rows ? rows : 0,
          .sx = sx,
          .sy = sy,
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

/* One frame's worth of "what covers what": the visible floats, so every
 * leaf's placements can be clipped by the ones above it. */
struct gfx_walk {
  app_t *a;
  rect_t occ[64];
  uint32_t raised[64];
  size_t n;
};

static void gfx_leaf_cb(node_t *n, void *ud) {
  struct gfx_walk *w = ud;
  if (n->hidden || n->collapsed) return; /* not drawn: images included */
  /* Above this leaf: every float for a tiled pane, the higher-raised for a
   * float — the same order draw_floats paints in, so an image is clipped by
   * exactly what its cells are covered by. */
  rect_t occ[64];
  size_t k = 0;
  for (size_t i = 0; i < w->n; i++)
    if (!n->floating || w->raised[i] > n->raised) occ[k++] = w->occ[i];
  struct gfx_ctx ctx = {w->a, n, occ, k};
  pane_graphics(n->pane, gfx_from_pane, &ctx);
}

/* Walk the visible tab's panes and produce the bytes the client's terminal
 * needs this frame. Borrowed until the next call. */
const char *app_graphics(app_t *a, size_t *len) {
  gfx_begin(a->gfx);
  if (a->ntabs && cur(a)->root) {
    struct gfx_walk w = {.a = a};
    node_t *fl[64];
    size_t nf = collect_floating(cur(a)->root, fl, 64, 0);
    for (size_t i = 0; i < nf && w.n < 64; i++) {
      if (fl[i]->hidden || fl[i]->collapsed) continue;
      w.occ[w.n] = fl[i]->rect;
      w.raised[w.n++] = fl[i]->raised;
    }
    walk(cur(a)->root, gfx_leaf_cb, &w);
  }
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

/* ---- the splash ---------------------------------------------------------- *
 *
 * The logo, briefly, centered over everything, when a client attaches: a
 * greeting, and a two-second advertisement for what the shader pass can do.
 * Which effect it wears is picked by the splash's own timestamp, so every
 * attach looks a little different without anybody keeping a counter. Any key
 * or click ends it (app_event), and the pass machinery is exactly the one
 * panes use -- same expressions, same clock, same cache. */

#include "logo.h" /* generated from logo.txt; see the Makefile */

typedef struct {
  const char *kind; /* tint or dim: colour it, or carve it */
  uint32_t rgb;     /* the colour, spelled 0xrrggbb; unused by dim */
  const char *amount;
} splash_pass_t;

/* A splash effect is a *chain*, because one tint is one colour and a greeting
 * in a single hue is a screensaver from a sadder time: phase-shifted passes
 * mix per cell, which is how a fixed-colour pass machinery does rainbows.
 * The colours are literal and loud on purpose -- this is the one place that
 * does not defer to the theme, because it is branding wearing a demo, not
 * chrome.
 *
 * Every pass runs on the foreground only. The logo is half-block art, so its
 * visible pixels *are* the foreground and the notches are the terminal's own
 * background -- and a pass that painted backgrounds turned the art into a
 * slab with a logo-shaped hole. Foreground-only also keeps the cleared
 * backdrop dark for free: a blank cell has no glyph for a colour to land on. */
#define SPLASH_CHAIN 4
typedef struct {
  splash_pass_t p[SPLASH_CHAIN];
} splash_fx_t;

/* Every entry reads `t` or `since`, deliberately: an animated splash keeps
 * the frame clock only while it is on screen (note_animation), and a still
 * one would just be a logo. `since` here is milliseconds since the splash
 * began, so one-shot effects can sweep exactly once. */
static const splash_fx_t SPLASH_FX[] = {
    /* spectrum: rainbow bands rolling across the glyphs */
    {{{"tint", 0xff2d95, "128 + sin(x * 9 - t / 3) / 2"},
      {"tint", 0x00e5ff, "128 + sin(x * 9 - t / 3 + 120) / 2"},
      {"tint", 0xffd400, "128 + sin(x * 9 - t / 3 + 240) / 2"}}},
    /* plasma: two interference patterns in clashing hues */
    {{{"tint", 0xff2d95,
       "128 + sin(x * 21 + t / 4) / 4 + sin(y * 47 - t / 6) / 4"},
      {"tint", 0x2de5ff,
       "128 + sin(x * 17 - t / 5) / 4 + sin(y * 61 + t / 7) / 4"}}},
    /* fire: heat rising through the glyphs */
    {{{"tint", 0xff3b00, "clamp(y * 34 + sin(x * 53 + t / 3) / 6, 0, 255)"},
      {"tint", 0xffc400,
       "max(0, (y - rows / 2) * 40 + sin(x * 97 - t / 2) / 5)"}}},
    /* rings: rainbow ripples spreading from the middle */
    {{{"tint", 0x7a5cff,
       "128 + sin(dist(x, y, cols / 2, rows / 2) * 55 - t / 2) / 2"},
      {"tint", 0x00ffa8,
       "128 + sin(dist(x, y, cols / 2, rows / 2) * 55 - t / 2 + 120) / 2"},
      {"tint", 0xff2d95,
       "128 + sin(dist(x, y, cols / 2, rows / 2) * 55 - t / 2 + 240) / 2"}}},
    /* glitch: a left-to-right reveal under two colours of static */
    {{{"tint", 0x00e5ff, "((x * 31 + y * 83 + (t / 70) * 47) % 89 < 9) * 255"},
      {"tint", 0xff2d95, "((x * 53 + y * 29 + (t / 90) * 31) % 97 < 9) * 255"},
      {"dim", 0, "(x * 500 > since * cols) * 255"}}},
    /* aurora: slow curtains of green and violet drifting over cyan */
    {{{"tint", 0x19ff8c,
       "128 + sin(x * 13 + sin(y * 40 + t / 9) / 8 + t / 6) / 2"},
      {"tint", 0x8c5cff, "128 + sin(x * 11 - t / 8 + 90) / 3"},
      {"tint", 0x00d0ff, "max(0, sin(x * 7 + t / 5) / 2)"}}},
};
#define NSPLASH_FX (sizeof SPLASH_FX / sizeof *SPLASH_FX)

/* ---- the particle engine ------------------------------------------------- *
 *
 * Every non-space glyph of the logo is a particle that flies to its cell over
 * the first two fifths of splash_ms, then *is* the logo for the rest. There
 * is deliberately no particle state anywhere: a particle's position is a pure
 * function of (target, elapsed, seed), recomputed every frame like the layout
 * and the pane states and for the same reason -- physics that is stored can
 * drift, physics that is derived cannot. The seed is the splash timestamp, so
 * every attach flies differently and a single splash is coherent frame to
 * frame; a per-particle hash of it staggers departures and scatters spawns. */

typedef struct {
  int tx, ty;    /* target, in screen cells */
  char glyph[8]; /* one UTF-8 cluster */
} splash_p_t;
#define SPLASH_P_MAX 512

/* The logo as particles, re-derived per frame: ~300 tiny copies against a
 * struct someone has to keep in sync with logo.txt is no contest. */
static size_t splash_particles(splash_p_t *out, int x0, int y0) {
  size_t n = 0, nlines = sizeof LOGO / sizeof *LOGO;
  for (size_t ly = 0; ly < nlines; ly++) {
    const char *sline = LOGO[ly];
    int cx = 0;
    for (size_t off = 0; sline[off] && n < SPLASH_P_MAX; cx++) {
      unsigned char lead = (unsigned char)sline[off];
      size_t len = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
      if (sline[off] != ' ') {
        out[n].tx = x0 + cx;
        out[n].ty = y0 + (int)ly;
        memcpy(out[n].glyph, sline + off, len);
        out[n].glyph[len] = 0;
        n++;
      }
      off += len;
    }
  }
  return n;
}

/* Ease-out cubic in 0..256 fixed point: fast off the line, gentle landing. */
static int splash_ease(int p8) {
  int q = 256 - p8;
  return 256 - q * q / 256 * q / 256;
}

/* This particle's progress, 0..256. `delay` (0..255) eats the front of the
 * window and the remainder is rescaled, so stragglers still land exactly at
 * the end -- an assembly that is only mostly assembled reads as a bug. */
static int splash_p8(int64_t since, int asm_ms, int delay) {
  if (asm_ms <= 0) return 256;
  int p = (int)(since * 256 / asm_ms);
  if (p >= 256) return 256;
  if (delay > 224) delay = 224;
  p = (p - delay) * 256 / (256 - delay);
  return p < 0 ? 0 : p > 256 ? 256 : p;
}

/* The motions. Each computes where particle i is *from* and how late it
 * leaves; the flight itself is one shared eased lerp, plus whatever offset
 * the motion adds along the way (the vortex's orbit). Coordinates may leave
 * the screen; the draw clips. */
enum {
  SPLASH_MO_RAIN,    /* fall in, column by column */
  SPLASH_MO_SCATTER, /* converge from everywhere */
  SPLASH_MO_SLIDE,   /* rows enter from alternating sides */
  SPLASH_MO_VORTEX,  /* spiral in around the centre */
  SPLASH_MO_SHUFFLE, /* the glyphs trade places, then sort themselves */
  NSPLASH_MO
};

static void splash_place(int motion, const splash_p_t *p, size_t i, size_t n,
                         const splash_p_t *all, uint32_t seed,
                         const screen_t *s, int64_t since, int asm_ms, int *ox,
                         int *oy) {
  uint32_t h = ((uint32_t)i * 2654435761u) ^ (seed * 2246822519u);
  int sx = p->tx, sy = p->ty, delay = 0;
  switch (motion) {
  case SPLASH_MO_RAIN:
    sy = -1 - (int)(h % 12);
    delay = (int)(h % 160);
    break;
  case SPLASH_MO_SCATTER:
    sx = (int)(h % (s->cols ? s->cols : 1));
    sy = (int)((h >> 9) % (s->rows ? s->rows : 1));
    delay = (int)((h >> 18) % 96);
    break;
  case SPLASH_MO_SLIDE:
    sx = (p->ty % 2) ? (int)s->cols + 2 + (int)(h % 8) : -3 - (int)(h % 8);
    delay = (p->ty * 31) % 128;
    break;
  case SPLASH_MO_SHUFFLE: {
    /* Everybody starts on somebody else's cell: a fixed rotation of the
     * particle list, which is a permutation by construction. */
    size_t j = (i + 1 + seed % (n > 1 ? n - 1 : 1)) % n;
    sx = all[j].tx;
    sy = all[j].ty;
    delay = (int)(h % 112);
    break;
  }
  case SPLASH_MO_VORTEX:
  default: delay = (int)(h % 64); break;
  }

  int e = splash_ease(splash_p8(since, asm_ms, delay));
  *ox = sx + (p->tx - sx) * e / 256;
  *oy = sy + (p->ty - sy) * e / 256;

  if (motion == SPLASH_MO_VORTEX && e < 256) {
    /* An orbit that shrinks and unwinds as the flight completes. dist counts
     * a row double everywhere else, so the y radius is halved here too. */
    int r = (24 + (int)(h % 20)) * (256 - e) / 256;
    int ang = (int)(h % 360) + e * 2;
    *ox += r * expr_sin(ang + 90) / 255;
    *oy += r * expr_sin(ang) / 510;
  }
}

static color_t splash_rgb(uint32_t rgb) {
  return (color_t){.set = true,
                   .r = (uint8_t)(rgb >> 16),
                   .g = (uint8_t)(rgb >> 8),
                   .b = (uint8_t)rgb};
}

void draw_splash(app_t *a, screen_t *s) {
  if (!a->splash_until) return;
  int64_t now = now_ms_();
  if (now >= a->splash_until) {
    a->splash_until = 0;
    return;
  }

  size_t nlines = sizeof LOGO / sizeof *LOGO;
  uint16_t w = 0;
  for (size_t i = 0; i < nlines; i++) {
    uint16_t c = cells(LOGO[i]);
    if (c > w) w = c;
  }
  /* Two cells of air each side, a row above and below. A screen the box does
   * not fit on gets no greeting rather than a cropped one. */
  uint16_t bw = (uint16_t)(w + 4), bh = (uint16_t)(nlines + 2);
  if (bw > s->cols || bh > s->rows) return;
  uint16_t x0 = (uint16_t)((s->cols - bw) / 2);
  uint16_t y0 = (uint16_t)((s->rows - bh) / 2);

  /* A cleared backdrop, so the logo reads over whatever a pane put there. */
  char blank[512];
  size_t nb = bw < sizeof blank - 1 ? bw : sizeof blank - 1;
  memset(blank, ' ', nb);
  blank[nb] = 0;
  for (uint16_t y = 0; y < bh; y++)
    screen_text(s, x0, (uint16_t)(y0 + y), blank, NO_COLOR, NO_COLOR, 0);
  /* The glyphs, wherever their flight has them this frame. White, not the
   * accent: the effect passes below paint the hues, and white is the canvas
   * that takes every one of them at full saturation -- tinting an
   * already-blue glyph towards pink lands on mud. Assembly takes the first
   * two fifths of splash_ms; from then on every particle sits on its target
   * and this is exactly the static logo. */
  splash_p_t parts[SPLASH_P_MAX];
  size_t np = splash_particles(parts, x0 + 2, y0 + 1);
  int64_t since = now - (a->splash_until - CFG.splash_ms);
  int asm_ms = (int)CFG.splash_ms * 2 / 5;
  uint32_t seed = (uint32_t)a->splash_until;
  int motion = a->splash_motion >= 0 ? a->splash_motion % NSPLASH_MO
                                     : (int)((seed / NSPLASH_FX) % NSPLASH_MO);
  for (size_t i = 0; i < np; i++) {
    int px, py;
    splash_place(motion, &parts[i], i, np, parts, seed, s, since, asm_ms, &px,
                 &py);
    if (px < 0 || py < 0 || px >= s->cols || py >= s->rows) continue;
    screen_text(s, (uint16_t)px, (uint16_t)py, parts[i].glyph, CFG.default_fg,
                NO_COLOR, ATTR_BOLD);
  }

  /* The effect chain. Programs are compiled on first wear and kept for the
   * life of the process: the sources are string literals above, and a splash
   * that recompiled its expressions every frame would be spending the one
   * cost the map exists to avoid. */
  static expr_prog_t *progs[NSPLASH_FX][SPLASH_CHAIN];
  size_t idx = a->splash_fx >= 0 ? (size_t)a->splash_fx % NSPLASH_FX
                                 : (size_t)a->splash_until % NSPLASH_FX;
  const splash_fx_t *fx = &SPLASH_FX[idx];
  shader_t chain[SPLASH_CHAIN];
  size_t nc = 0;
  for (size_t i = 0; i < SPLASH_CHAIN && fx->p[i].kind; i++) {
    if (!progs[idx][i]) {
      char err[128] = {0};
      progs[idx][i] = expr_compile(fx->p[i].amount, err, sizeof err);
    }
    if (!progs[idx][i]) continue; /* cannot happen; skip the pass, not all */
    shader_make(&chain[nc], fx->p[i].kind, splash_rgb(fx->p[i].rgb), 128);
    chain[nc].channels = SHADE_FG; /* the art's pixels; see the table's note */
    chain[nc].amount_expr = progs[idx][i];
    nc++;
  }
  if (!nc) return; /* the bare logo still greets */

  shade_ctx_t ctx = {
      .now_ms = now,
      .state_ms = since, /* the splash's own age, for one-shot effects */
      .default_fg = CFG.default_fg,
      .default_bg = CFG.default_bg,
  };
  note_animation(a, chain, nc);
  shade_apply(s, chain, nc, (rect_t){x0, y0, bw, bh}, NULL, &ctx);
}
