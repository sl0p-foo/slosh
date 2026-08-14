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

**The test harness is not a session, and the difference hid a real bug.** The
server set `signal(SIGCHLD, SIG_IGN)`, which does not merely skip a handler —
it tells the kernel to discard exit statuses, so `waitpid` can never report
one. The headless driver never set it, so every test agreed that a dead pane
knew why it died, and the first real session showed `[process exited]` with no
status at all. Found by looking, in a zellij pane, thirty seconds after the
suite went green. If something is right in tests and wrong in front of you,
suspect what `server.c` does that `headless.c` does not.

**A dead pane's fd is closed at EOF, deliberately.** Panes used to be reaped
before the next paint, so nothing had to think about it. Now that they stay,
an EOF fd left in the poll set is *readable forever*: the session would spin a
core behind a pane that looks idle. `collect_cb` skips `fd < 0` for the same
reason, and that is why `sl0ppty run cmd` still terminates — the settle loop
ends when no pane has an fd left.

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

## Dead panes (D14), and what it changed

A pane now outlives its program: it keeps its contents, writes
`[process exited: status 3]` into its own backlog, and offers `[re-run]` and
`[close]` in its bottom frame row. Three things about it are worth knowing
before changing anything near it.

- **The dead row and the OSC 5577 row are the same row**, by choice. A dead
  pane's own buttons are inert — a click would write into a closed pty — so
  `draw_pane_status` swaps the list rather than adding a second mechanism.
  `close` is listed last so it is drawn rightmost and therefore survives a
  narrow frame; the thing that drops off is `re-run`.
- **Re-running is the same pane.** Same id, same node, same terminal, so
  nothing that referred to it needs telling and the previous run stays above
  in the scrollback. That is the whole point; do not be tempted to close and
  reopen.
- **The exit status is collected with a bounded wait, never a blocking one.**
  EOF means the program closed its fds, which for an exit already happened, so
  a few hundred microseconds wins the race with the kernel's exit path. A
  blocking `waitpid` would hang the entire session on one program that closes
  its terminal and keeps running. Losing the race costs the words "status 0".

## Shaders: what is measured, and what is not worth doing

Before optimising anything here, read the numbers rather than reasoning about
them — `.scratch/shbench.c` and `.scratch/shprof.c` produce them in a second
(they are gitignored; rebuild with the command in their headers).

What they say, per cell per pass on this box:

- the **arithmetic is free**. `vignette` — squared distances, clamp, divide —
  is *cheaper* than `dim`, which computes nothing. So precomputing an amount
  per cell, the obvious optimisation, buys approximately zero.
- the cost is per-cell overhead and the colour mixing. Removing `screen_at()`
  from the inner loop took the pass from 8.0ns to 6.1ns for ten lines, and
  that is done.
- the next real win is **per-row dispatch** (an indirect call per row rather
  than per cell, which also lets each shader hoist its row-invariant work):
  measured at 4.7ns in a prototype. Not done, because it is a rewrite of every
  shader and nothing needs the 1.4ns yet.
- an **interpreted** shader is 56ns against a compiled 8ns, which is the whole
  argument for D15 loading native code instead of inventing a language.

All of this is 0.24ms per frame on a 200x50 pane, under 3% of a 120Hz frame.
It has never been the bottleneck; do not spend a day here without a profile
saying otherwise.

## Things left on the table

- **A `reload` keybinding.** The config watcher made it less pressing.
- **Tab-level shaders**, and the bigger one: **OSC 5577 → shader**, letting a
  pane colour itself by reporting its own state. That is the idea most likely
  to make this feel unlike other multiplexers.
- **Collapsed headers do not reach the shader pass**, so a `suspended`,
  `dead` or `scrolled` colour is invisible on one. This is now the most
  visible gap: a tab that has flattened into a list is exactly where you would
  want a dead pane to stand out, and it is the one place it does not.
- **A dead pane cannot be re-run from the finder or the minimised bar** — only
  from its own frame, `C-a r`, or `{"cmd":"rerun"}`. Fine while a dead pane is
  something you are looking at; less fine once you have six of them put away.
- **Nothing re-runs a whole tab**, which is the obvious next want once one
  pane can be re-run.
- **Shader plugins cannot be replaced without a new session** (D15): loading
  is additive on purpose, because a `shade_fn` may be live in a config or on a
  pane. Hot-swapping would need every shader reference to be indirected
  through the registry, which is a real change and not obviously worth it.
- **Per-row shader dispatch** — see the numbers above.
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
