# Floating panes — plan

Status: **M10a and M10b built and green** (56 checks in
`tests/test_float.py`, full suite passing). When agreed, the decision below
becomes D22 and the milestone becomes M10 in DESIGN.md.

A floating pane renders on top of the tiled panes — below the modals, the
toasts and the splash — and can be freely moved and resized. The claim this
plan defends: **almost all of it already exists**, because the two structural
commitments (one geometry; layout as a pure function) were designed for
exactly this shape of feature. The hit list *is* a z-order, and intent-plus-
derivation *is* how a remembered rect survives a resize.

## The model: a flag and a rect, like `minimized`

A floating pane is **a leaf still in the tree**, carrying two new pieces of
intent:

```c
bool floating;      /* out of the layout, drawn on top */
rect_t float_rect;  /* where it wants to be, in cells; intent, never clamped
                       in place */
uint32_t raised;    /* monotonic stamp; highest paints last (= on top) */
```

Not a separate per-tab list. `minimized` already proved the pattern: the leaf
keeps its seat in the tree, `layout_node` skips it, the siblings absorb its
share, and un-floating returns it home — same id, same place, nothing that
referred to it has to be told anything (the D14 property). Everything that
walks leaves — `pane_by_id`, the finder, `close_leaf`, `dump-layout`, the D6
flatten — keeps working with zero new cases, because there is no second
collection to forget to walk.

Rules that fall out of reusing the pattern:

- **The last tiled pane cannot float**, same guard and same reason as
  minimise: a tab that is only an overlay is an overlay over nothing.
- **Floating and minimized compose.** A minimized float sits in the strip like
  anyone; restoring it restores it floating. Two orthogonal flags, no matrix.
- **Closing a float is `close_leaf`**, unchanged. Its tree seat collapses like
  any leaf's.

### The drawn rect is derived, the wanted rect is remembered

`float_rect` is what you asked for. What is painted each frame is
`clamp(float_rect, tab_area)` — inside the tab's content area (below the tab
strip, above the status line; those rows own global verbs and nothing may
cover them), floored at `MIN_PANE_COLS/ROWS` plus the frame. The clamp **never
writes back**: shrink the terminal and the float is squeezed in; grow it again
and the float is exactly where you put it. Same rule as the layout tree's
rects — recomputed every frame, never trusted between them.

Floating an existing tiled pane seeds `float_rect` from its current rect inset
by one cell per side, so the transition reads as the pane lifting off the page
rather than teleporting.

### Z-order is the raise stamp, and the hit list makes it honest

Floats paint in ascending `raised`; focusing a float stamps it. That is the
whole z-machine. Because `hit_test` searches the hit list **backwards**, the
float painted last owns the topmost hits automatically — a click lands on
what you see, with no routing code written. The modals still paint after the
floats, so a picker over a float behaves today's way for free. This is the
"one geometry" commitment paying out: the z-order cannot disagree with the
click order because they are the same list.

Draw order in `app_compose` becomes:

```
tab strip → tree → corners → min bar → status line
→ floats, in raise order                          ← new
→ scrim → picker/help → toasts → splash
```

## Input

**Mouse.** All from the hit list, like the existing four drag verbs:

| target on a floating frame | drag does |
|---|---|
| the title row | **move** the float (`DRAG_FLOAT_MOVE`) — not swap |
| any border cell / corner | **resize** from that side (`DRAG_FLOAT_RESIZE`) |

A press that never moves is a click (the distinction the drag machine already
makes): click anywhere on a float focuses and raises it. The ▬ □ ✕ buttons
keep their verbs. A float's frame registers `floatmove:`/`floatresize:`
actions instead of `border:`/`edge:`, so the split guide never arms on it —
derived from the hit list, so it cannot half-arm. **Splitting a float is
refused with a word**: a split is a statement about the tree's arrangement,
and a float is precisely not arranged. Any keystroke ends a drag, as today.

**Keyboard.** One toggle action, `float`, reachable from the palette on day
one (D19 gives that for free). For a focused float, the existing resize
family gets a second meaning the same way it already means "move the
boundary of whatever is focused": `H J K L` **moves** the float by a step;
resize gets its own chord (open question below). `h j k l` stays focus
movement — floats participate in `focus_dir` geometrically, by their drawn
rects, and in `focus_next` by tree order, both for free.

## Interactions with what exists

- **Zoom**: unchanged. A zoomed pane fills the tab whether it floats or not
  (zoom reuses the solo path); unzooming a float returns it to `float_rect`.
- **D6 flatten**: a flattened tab lists floats with everyone else — they are
  leaves in the tree, so the flatten already includes them. A screen too
  small for tiles is too small for overlays; `float_rect` survives untouched
  for when room returns.
- **Shader states (D13/D20)**: a new `floating` pane state in the table, so a
  float can be told apart by colour. Ranked ambient, below `bell`. Ships no
  default tint (a float is live; nothing needs explaining) — the *shadow*
  does that job instead: one cell of dimming at (+1,+1) outside the frame,
  painted by the float pass over cells already composed. Derived every frame,
  knob `float_shadow`, default on.
- **Kitty graphics (D17)** — the one genuinely new cost. The cell diff gets
  occlusion right by paint order, but placements are emitted *after* the
  diff and would paint an image from a tiled pane over a float covering it.
  v1 rule: a placement clipped by a float from one clean side is **cropped**
  (the source-rectangle machinery from D17, pointed at a second clipper);
  a float landing in the *middle* of an image is a shape one placement
  cannot express, so that placement is **suppressed for the frame** and
  returns when the float moves. A float's own images already work — cropping
  at the pane edge is the same code — and are emitted with a kitty `z=` above
  the tiled panes' placements, matching the cells.
- **Connections/clients, D10**: untouched; this is all server-side compose.

## Persistence and the control API

- **Layout files (D2)**: pane nodes take `floating=true x= y= w= h=`, both
  loaders, both schemas linted the usual way. `dump-layout` writes it, so
  `save-workspace` (D21) records a float and a restored session floats it.
- **Control API (D3)**: `{"cmd":"float","id":N}` toggles;
  `{"cmd":"float","id":N,"x":..,"y":..,"w":..,"h":..}` places. `panes`
  reports `floating` and the rect. Bare-verb alias runs the same code.

## Tests (headless, per the harness)

- float a pane → snapshot: its cells overwrite the tiled cells under it;
  hit list resolves the overlap to the float.
- two floats → focus the lower one → it is on top next frame (raise = focus).
- drag-move and drag-resize via `send` of SGR mouse; a keystroke mid-drag
  ends it.
- shrink the terminal under a float → clamped; grow it back → **exact**
  original rect (the fork's collapse/expand lesson, asserted for floats).
- flatten a tab containing a float → the float is a row, reachable.
- dump → apply → the float is back, floating, placed.
- refuse: floating the last tiled pane; splitting a float.

## Milestone M10, staged

1. **M10a — the model.** ✅ Flag, rect, stamp; layout skips it; draw pass after
   the status line; clamp; focus/raise; `float` action + palette; the guard
   rules; tests above minus the drags. Three deltas found by building it:
   - A bare `{"cmd":"float","id":N}` toggle shipped early (the harness is a
     client; placement arguments stay M10c).
   - The backdrop invariant needs a landing rule, not just a guard: closing
     or moving away the last tiled pane un-floats the top float
     (`ensure_tiled`, checked once in `layout()` like the minimised-focus
     rule). The minimise guard tightened to match: another float is not
     something to show.
   - A float refuses splits through `split_fits` returning false, which
     reaches the guide, the border click and the keyboard floor check in one
     place; the keyboard, the border release and the API each add the words
     ("a floating pane cannot be split").
2. **M10b — the hands.** ✅ `DRAG_FLOAT_MOVE`/`DRAG_FLOAT_RESIZE`, keyboard
   move, the shadow, the `floating` shader state. What building it decided:
   - **A grabbed edge pins at the wall.** The general intent clamp preserves
     the size by sliding the rect, which turns "drag the right edge past the
     wall" into the whole window walking left — so `float_resize` clamps each
     grabbed edge against the tab area itself and never moves the opposite
     one. Which edges are held is derived from where the press landed on the
     rect painted that frame, so a bottom corner is two edges for free.
   - **Explicit moves write intent back.** A drag or keyboard nudge applies
     its delta to the rect *as drawn* and, after the layout clamps, writes
     the clamped answer back into `float_rect`: the hand is the author of the
     intent, so "where you see it is where it stays". A terminal resize still
     never writes back. Refused while the float is not drawn as a float
     (flattened or zoomed), where writing the shown rect back would destroy
     the place it returns to.
   - **The float verbs are their own drag kinds**, not flags on
     `DRAG_TITLE`/`DRAG_BORDER`: everything those mean on release — swap,
     split — is exactly what a float must not do. The rename double-click on
     a float's name still wins, and a float is excluded as a swap *target*
     too (its tree seat is not where it is).
   - **`floating` ranks above `unfocused`**, so a float is never dimmed by
     `dim_unfocused`: full strength is what keeps the thing on top reading
     as lifted — the `dragging` argument. It ships no chain; the shadow does
     the telling apart.
   - **The shadow is a number, not a state** (`float_shadow`, default 110,
     0 for off) — the `dim_unfocused` argument about discoverability. Cast
     as the scrim's own dim pass over two non-overlapping strips
     (`gap_aspect` columns beside, one row below, offset so the light comes
     from the top left), before the float paints, so it falls on lower
     floats too.
   - `H J K L` on a focused float move it one gap-aspect-square step;
     keyboard *resize* of a float remains open (question 2). A float's title
     also does not target the tab strip yet — moving between tabs stays
     `pane-to-next-tab`'s job for now.
   - **The border says it is a handle** (added after a hand went looking):
     hovering lights the edge a grab would move in the resize colour, with
     the `⇔`/`⇕` arrow at the grab point; a corner lights both edges; the
     held edges stay lit through the drag; the status hint says "drag to
     resize (both ways)" and a float's title handle stops promising the
     split it does not offer. All read from one derivation
     (`float_edges_at`) shared with the press, so the promise, the paint
     and the drag cannot disagree. Finding this also found an ordering bug:
     the status line — which reads the hit list for its hover hint — was
     painted *before* the floats, so a float's border hinted as the pane
     underneath. Floats now paint before the status line (their rects
     cannot overlap; the clamp sees to it), which also keeps a float's
     shadow off the status row on a gap-less config.
3. **M10c — the memory.** Layout schema, dump, control API, the graphics
   occlusion rule.

## Open questions

1. ~~**The default key.**~~ **Resolved: `C-a f` floats.** Both float and the
   finder wanted the letter; the tie went to the verb — f is what "float"
   sounds like, and a float toggle is pressed in a flow (lift, look, put
   back) where a reach hurts. The finder moved to `C-a s`, where it reads
   just as naturally as *search*.
2. ~~**Keyboard resize chord**~~ **Resolved: the =/+ key grows, `-`
   shrinks**, about the float's own centre so it stays under your eyes.
   Grow is bound with and without shift because a legacy terminal reports
   `+` as a bare `=` (the `?`/`/` deal), which is also why **equalize moved
   to `0`** rather than sharing the key: two verbs on one key survive only
   until a terminal drops the shift — and `0` reads as "reset the shares"
   on the digit row the tabs already own. Shrink rides the split-rows case,
   where a float's split was a refusal anyway.
3. ~~**Docking**~~ **Resolved: unfloat is the dock.** The float kept its
   seat all along, so `C-a f` on a focused float already lands it — no drop
   gesture needed.
4. ~~**Creating panes floating**~~ **Resolved: `C-a F` / `new-float`** — the
   throwaway terminal. A real leaf beside the focused pane (so un-floating
   lands it there and `exit` closes it under D14's shell rule), born
   floating at the centred default size, in the focused pane's directory.
   `f`/`F` is a shifted pair: float this pane / float a fresh one. (It
   spent a day on F12, the IDE key for the same idea, and came back — a
   leader-and-letter chord is what every other verb costs, and the function
   row is a reach the home row is not. The detour still paid: `f1`..`f12`
   joined NAMED_KEYS, so the function row is bindable by any config — the
   decoder always understood it, the config just had no names for it.) The
   layout-file case stays M10c.
5. **Graphics crop vs suppress** — start with the simple rule and let a real
   program complain.

## A later decision: the pop to the centre

Floating a pane now **always centres it** (M10b originally lifted it in
place, inset a cell). The jump is what says the float *happened* — a pane
lifted in place looks like a pane whose neighbours flinched. The size is
still the one remembered from the last time a hand shaped it (or seeded
from the pane's tiled rect); only the position is opinionated, because the
position is the announcement. This traded away "refloat returns to where
you parked it" deliberately: position is re-announced, size is remembered.
