# Handover

Written at the end of a long session, for whoever picks this up next — most
likely me, without the memory of having written any of it.

`DESIGN.md` says what this is and why. `README.md` says what it does.
`.fastfeedback/SPEED.md` says how to run things quickly. This file is the part
that is only learnable by having got it wrong.

## Where things stand

Everything is committed and pushed; `git log` is the honest changelog and the
messages are long on purpose. The working tree should be clean — if it is not,
that is unfinished work and not a stash worth trusting.

Built and tested with `make test` (~6s, parallel, per-file stamps) and
`make test-live` (~7s, needs real ptys). Both green. Clean build, no warnings.

## The rule this codebase keeps, and how I kept breaking it

**Anything derived from the layout is recomputed every frame and must not be
remembered between frames.** `rect`, `content`, `collapsed` and `hidden` all
say so in their comments. The corollary is the part that bites: *intent* is
different and does have to be stored — `weight`, `minimized`, a tab's `zoom`,
which pane has focus.

Three bugs in one afternoon came from confusing the two:

- A drag held an **index into a corner list rebuilt every frame**, by a drag
  whose whole job was changing the layout. It silently started moving other
  panes' boundaries. Fixed by storing the boundaries themselves.
- `layout_node` clears `collapsed`/`hidden` at entry. Putting `minimized` next
  to them would have un-minimised everything every frame; it lives beside
  `weight` instead, with a comment saying why.
- A test counted visible panes across *all* tabs. Only the current tab is laid
  out, so every other tab's rects are whatever they were last time it was on
  screen.

If something works and then stops working while you drag or resize, this is the
first thing to suspect.

## Traps that cost real time

**The dwell repaint list.** `app_next_deadline_ms` lists the hit-action
prefixes that arm after the pointer rests. Anything that arms on dwell and is
*not* listed there appears only when some unrelated event happens to repaint —
so it looks intermittent, which is the worst way to be wrong. This caught me
twice, for `edge:` and then `corner:`.

**Glyph width and glyph weight are different problems.** `screen_text` books
every chrome cell as one column. An emoji drawn two columns wide shifts the
whole row. Separately, a glyph that does not *fill* its cell donates that space
to the gap beside it and looks badly spaced next to one that does — the cells
can be provably even and still look wrong. Three attempts at the frame buttons
established this; they are plain ASCII now. If someone reports spacing, dump
the actual cells before touching any arithmetic.

**`settle N` in the test harness is a sleep in disguise.** `send` and
`snapshot` are already synchronous with the app, so a settle before asserting
on *our own chrome* waits for nothing. It is only needed to pump a pane's pty.
Use `until_text()` where the answer is observable. Getting this wrong is what
made the suite 80 seconds; it is 6 now.

**Tests pinned to incidental coordinates.** Several asserted a style at a
hardcoded column, or a shape that happened to be true. They only break when
something unrelated moves, and then they look like real regressions. Assert the
thing you mean.

## Architecture in five sentences

`app.c` owns the layout tree, drawing, and input routing; `pane.c` wraps one
pty plus one libghostty-vt terminal; `screen.c` is a cell buffer that diffs
itself and emits the minimal byte stream. Everything painted registers its
`(rect, action)` into a hit list *as it is painted*, and a click is a lookup in
that list — this is D1 and it is why drawing and hit-testing cannot disagree.
Layout runs twice a frame: once as a probe to ask whether the tab fits, once
for real, and the probe must not resize panes. Shaders are a colour pass over
pane *contents* between `pane_compose` and the chrome drawn over them, which is
what makes "contents, not chrome" the paint order rather than a rule. The
control API in `cmd.c` is the same vocabulary the headless test driver speaks,
so a script written against one works against the other.

## Things left on the table

- **A `reload` keybinding.** The config watcher made it less pressing.
- **Tab-level shaders**, and the bigger one: **OSC 5577 → shader**, letting a
  pane colour itself by reporting its own state. That is the idea most likely
  to make this feel unlike other multiplexers.
- **Dead panes are not observable.** `app_reap` closes a pane before the next
  paint, so there is no `dead` pane state to colour. Making one means keeping
  dead panes until dismissed — a feature about lifetimes, not colour.
- **Collapsed headers do not reach the shader pass**, so a `suspended` or
  `scrolled` colour is invisible on one.
- **Corner crossings**: found for both nestings now, but if more turn up
  missing, ask *how the layout was built* — the split order decides the tree
  shape, and that is where any remaining blind spot will be.

## How to work on it

Read the comment above the thing before changing it; they explain the decision,
not the mechanics, and several are load-bearing. Run `make test` constantly —
it is six seconds. Drive the thing in a zellij pane and look at it; every bug
in this file was found by looking, not by reasoning. Write the test that would
have caught it, and check the test fails against the old build before believing
it.
