# Panes and tabs

A tab is a tree of splits. Panes keep running in every tab; only the one you are
looking at is drawn.

## Arranging

- **Split** with `C-a Enter` and it picks the axis: across the pane's longer side,
  measured as you see it (a cell is about twice as tall as it is wide, which is
  what `gap_aspect` says), falling back to the other axis when that one will not
  fit and saying which way it went. `C-a \` and `C-a -` when you mean one.
  Splitting the same direction again gives three equal shares rather than
  1/2 + 1/4 + 1/4.
- **A new pane starts in the directory the pane it came out of is in**, not where
  the session was started. Splitting inside a project to run one more thing and
  landing in whatever directory the server was launched from is wrong on its own;
  it was wrong on disk as well, because a saved
  [project layout](workspaces.md) then carried an absolute `cwd=` pointing at
  somewhere else entirely.
- **Resize** with `C-a H J K L`: the boundary moves the way you press, whichever
  side of it you are on. Sizes are *weights*, so a resized layout survives the
  window changing size.
- **`C-a =`** gives every visible pane an even share again — each split divided
  between the panes behind it, so one pane beside a stack of three is a quarter
  of the width rather than half of it.
- **`C-a Space`** turns the whole tab a quarter turn clockwise. Every split
  changes axis and a row split's children reverse, because the pane on top of a
  stack is the pane on the right once you turn it. Four turns come back exactly.
  A turn that would squeeze a pane under the floor is refused with a word.
- **`C-a z`** fills the tab with one pane and back; **`C-a m`** puts one away
  into a strip along the bottom, still running. Click its row to bring it back.

## The mouse

Every mouse verb starts from the same list of rects the drawing filled in, so
nothing you click can disagree with what is on screen.

- **Click the middle of a pane's border to split toward it** — the side you click
  is the side the new pane appears on. Only the middle of an edge splits: three
  to seven cells of it, called the handle, because a whole edge is a long target
  to brush past and a changed layout is an expensive accident. Resting anywhere
  on an edge fills the handle in solid, with an arrow in it for the way it will
  split — a cell wider on the left and right edges, and no taller on the top and
  bottom ones, since a row is about twice a column and a two-row bar reads as a
  wall. Resting *on* the handle adds a dashed line where the new boundary will
  land. The rest of the edge does nothing when clicked.
- **The top row is the drag handle, and its handle splits upward.** Clicking a
  pane's header to focus it never splits it. Its handle goes as near the middle
  of the row as the row allows: centred when the row is empty, and against the
  title when the title is holding the middle, since the title keeps its own cells
  for the double-click rename. Arming an edge leaves the name, the buttons and a
  dead pane's epitaph legible — the heavy rule goes round them.
- **Drag the gap between panes** to move that boundary. Where two gaps cross,
  drag the crossing to move both at once.
- **Drag a pane by its title** onto another to swap them. Everything it could
  land on greys out; the pane in your hand keeps its colour.
- **...or onto a tab in the strip** to move it into that tab, or onto the `+` for a
  tab of its own. The tabs light up as destinations while you carry a pane and the
  one under the pointer fills; the tab it already lives in does not offer itself. A
  toast says where it went, since the pane lands somewhere you are not looking.
- **Name a pane and it stops listening to its program.** `pane_title` prefers a
  name while there is one, so a program that keeps announcing something stale —
  an agent still spinning the summary of a task it finished — is overruled by
  naming its pane, from the gesture below or from a script:
  `slosh cmd '{"cmd":"set-name","target":"pane","id":3,"name":"agent: gbos"}'`.
  An empty name hands the label back.
- **Double-click a name to rename it in place** — a pane's title or a tab in the
  strip. Enter keeps it, Escape abandons it, empty gives it back to the program.
  A name is not a [purpose](layouts.md#purposes): the purpose is a separate label
  with a key of its own, `C-a P`, so a rename and a tag cannot be confused with
  each other. The purpose editor draws in the same title cell but labels itself
  `purpose <what you type>`, so you can see which of the two you are editing.
- **Drag a tab along the strip** to reorder it. The strip rearranges as you go
  rather than dropping it at the end.
- Click a tab to switch, `+` to open one, a pane to focus it, and the marks in a
  frame's corner to minimise (`▬`), zoom (`□`) or close (`✕`) it.
- **A tab in the strip has no `✕`** and closes with `C-a X` instead. A mark per tab
  would cost two columns of every tab on every frame, forever, for a verb pressed
  rarely — the same arithmetic that took the per-pane `+` away. A pane's marks
  earn their cells because there is one set of them and it is where you are
  looking.
- Hovering anything says what it does, in a word, in the status line.

## Finding a pane

Tabs stop being navigation somewhere around six projects. `C-a f` opens a picker
over the whole session — every tab, including panes a small window has collapsed
out of sight. Type to narrow it by pane title, tab name or
[purpose](layouts.md#purposes); arrows, `C-n`/`C-p` or tab to move, `C-u` to clear
what you typed, enter to go.
A dot marks where you already are.

The finder searches what this session has; `C-a w` lists what is on disk —
every project under your roots, open or not ([workspaces](workspaces.md)). A
project you have not opened yet has no pane for the finder to match.

## Panes that were given a command

Split off a shell, do something, type `exit`: it closes, like a terminal should.
But a pane that was *told to run something* — from a [layout](layouts.md) or the
[control API](scripting.md) — keeps what it printed when that something exits,
with two lines saying what ran and how it ended, and two buttons:

```
 [ran: npm run dev]
 [process exited: status 3]
╰ exited: status 3 ───────────────────────────────[re-run]─[close]─╯
```

**The command is written down because `[re-run]` is one button.** Without it,
pressing it is a guess — most of all in a tab that a [project's
layout](workspaces.md) built, where the command was never typed into that pane to
be scrolled back to. A pane running the session's shell has no command to name
and gets the exit line alone.

`[re-run]` (or `C-a r`) runs the same command again in the same pane, keeping the
previous run above it in the scrollback. So the two notes turn a pane you re-run
into a log of what ran rather than a pile of identical epitaphs — and a command
that failed while you were looking elsewhere still has its error message when you
get back, while a mistyped command in a fresh session no longer closes it.

Which panes stay is one setting, `keep_dead`: `commands` (the default), `all`,
or `none`.

## Small terminals

When the panes no longer fit, the tab becomes a list of one-line headers with
the focused pane open below them — and returns to exactly the layout you had
when there is room again. Nothing is stored and restored: the arrangement is
recomputed from the tree every frame, and the list is what that function returns
when the room runs out.

`min_pane` is where that starts happening; `min_split` is the smaller pane a
split is willing to *create*. A split is refused when either floor would be
broken — including the axis it does not divide, since splitting cannot improve
that one: a pane already too short for two rows has no room for two columns
either, whatever its width says.

## Text, images, bells

- **Select text to copy it.** Release the mouse and it is on your clipboard,
  sent to your terminal over ssh as well as locally. Middle click pastes.
- **Double-click a word to select it**, and it is copied the same way a drag is:
  the highlight *is* the copy. What counts as one word is `word_separators`,
  which lists what a word stops at rather than what it is made of — so
  `src/app.c:1234`, `https://a.b/c?d=1`, `snake_case` and a run of CJK each come
  out whole, while a quote or a bracket ends one. A click on a blank cell or on
  a separator selects nothing and leaves your clipboard as it was.
- **Images work.** Kitty graphics pass through to your terminal, including the
  half of the protocol other multiplexers drop, where a program uploads an image
  once and places it every frame. Your terminal's cell size is carried through to
  each pane, so a program can size an image without saying how many cells to
  cover. PNG uploads are decoded here, because the terminal core ships no image
  decoder. (Sixel is not supported.)
- **A pane that beeps says so** — in its title, in its tab, and until you look at
  it. `bell_indicator false` makes a bell silent *and* invisible, which is a real
  choice. A [chrome shader](chrome.md) can make it louder.
