# Panes and tabs

A tab is a tree of splits. Panes keep running in every tab; only the tab you are
looking at is drawn.

## Arranging

- **Split**
  keyboard: `C-a Enter`
  mouse: hover near existing pane border in the middle to reveal clickable split actions.
- **A new pane starts in the directory the pane it came out of is in**, not where
  the session was started.
- **Resize** with `C-a` `H J K L`: the boundary moves the way you press. Sizes are _weights_,
  so a resized layout survives the window changing size. Of course you can also drag in the gutters of the panes
  to freely resize them! (or the pane's border in `compact` mode)
- **Equalize** panes using **`C-a 0`** (`equalize`), it will attempt to equalize the space between all panes.
- **Rotate your current layout with** **`C-a Space`** (`rotate_layout`); it turns the whole tab a quarter turn clockwise.
- You can (temporarily) **Zoom** in on panes with **`C-a z`** (`zoom`); it fills the tab with one pane and triggering it again unzooms.
- **Minimizing** panes can be done with **`C-a m`** (`minimize`); it minimizes the active pane into a strip along the bottom, still running.
  Each put-away pane sits there as a **miniature pane frame**, and clicking it anywhere brings the pane
  back. BEL notifications are drawn in the minitature pane frame as they arrive.

## The mouse

- **Click the middle of a pane's border to split toward it**
- **The top row is the drag handle, and its handle splits upward.** Clicking a
  pane's header to focus it never splits it. Its handle goes as near the middle
  of the row as the row allows: centred when the row is empty, beside the title
  when the title holds the middle (the title keeps its cells for the
  double-click rename). Arming an edge leaves the name, the buttons and a
  dead pane's epitaph legible: the heavy rule goes round them.
- **Drag the gap between panes** to move that boundary. Where two gaps cross,
  drag the crossing to move both at once. With `gap 0` and **[`compact true`](config.md#compact)**
  there is no gap to grab, so grabbing the border itself and pulling does the same thing.
- **Drag a pane by its title** onto another to swap them, or to _place_ it:
  the pane under the pointer subdivides into a centre and four edge bands,
  and where you let go is what the drop means.
- **...or onto a tab in the strip** to move it into that tab, or onto the `+` for a
  tab of its own.
- **Double-click a name to rename it in place**: a pane's title or a tab in the
  strip. Enter keeps it, Escape abandons it, `C-u` clears the field (a rename
  seeds with the current label, since it is usually an edit), empty gives it
  back to the program. The same two verbs are in the palette as `rename-pane`
  and `rename-tab`, for hands that are on the keyboard.
  A name is not a [purpose](layouts.md#purposes): the purpose is a separate label.
- **Drag a tab along the strip** to reorder it. The strip rearranges as you go
  rather than dropping it at the end.
- **Click a tab to switch**, `+` to open one, a pane to focus it, and the marks in a
  frame's corner to minimise (`▬`), zoom (`□`) or close (`✕`) it.

## Floating a pane

`C-a f` lifts a pane out of the layout and draws it on top of the tiled ones
(below the modals) where it can be moved and shaped freely. It **pops to
the centre of the tab**. Pressing `f` again puts it back in the seat it kept,
with the layout exactly as it was.

`C-a F` opens a **new floating shell**, the throwaway terminal, over
whatever you are doing, in the focused pane's directory; `exit` (or `^D`) closes
it like any shell, and un-floating lands it beside the pane it was opened over.

## Links

**OSC 8 hyperlinks pass through.** A program that emits real hyperlinks
(`ls --hyperlink`, gcc's diagnostics, delta) keeps them: the compositor
carries the link beside each cell and re-emits it to your terminal, which
offers it with its own gesture.

**Plain-text URLs are your terminal's own matcher**, running over what slosh
paints, with one catch worth knowing. While any program owns the mouse (a
multiplexer does), Ghostty only offers links when **shift** is added to the
usual gesture: shift is its mouse-capture escape, and it is stripped before
the link's own modifier is checked. So the gesture inside slosh (or tmux,
or zellij) is **shift+cmd+hover** to highlight and **shift+cmd+click** to
open (shift+ctrl on Linux). A URL that _wraps_ onto a second row cannot be
matched this way through any multiplexer, because the screen is repainted row
by row and the terminal sees two hard lines. That is exactly what OSC 8
links, which pass through whole, are for.

## Finding a pane

Tabs stop being navigation somewhere around six projects. `C-a s` opens a picker
over the whole session: every tab, including panes a small window has collapsed
out of sight. Type to narrow it by pane title, tab name or
[purpose](layouts.md#purposes); arrows, `C-n`/`C-p` or tab to move, `C-u` to clear
what you typed, enter to go.
A dot marks where you already are.

The finder searches what this session has; `C-a w` lists what is on disk,
every project under your roots, open or not ([workspaces](workspaces.md)). A
project you have not opened yet has no pane for the finder to match.

## Panes that were given a command

Split off a shell, do something, type `exit`: it closes, like a terminal should.
But a pane that was _told to run something_, from a [layout](layouts.md) or the
[control API](scripting.md), keeps what it printed when that something exits,
with two lines saying what ran and how it ended, and two buttons:

```
 [ran: npm run dev]
 [process exited: status 3]
╰ exited: status 3 ───────────────────────────────[re-run]─[close]─╯
```

`[re-run]` (or `C-a r`) runs the same command again in the same pane, keeping
the previous run above it in the scrollback: a pane you re-run becomes a log of
what ran, and a command that failed while you were looking elsewhere still has
its error message when you get back.

Which panes stay is one setting, `keep_dead`: `commands` (the default), `all`,
or `none`.

## Responsive Layout for small terminals

When the panes no longer fit, the tab becomes a list of one-line headers with
the focused pane open below them, and returns to exactly the layout you had
when there is room again.

`min_pane` is where that starts happening; `min_split` is the smaller pane a
split is willing to _create_. A split is refused when either floor would be
broken, including the axis it does not divide, since splitting cannot improve
that one: a pane already too short for two rows has no room for two columns
either, whatever its width says.

## Text, images, bells

- **Select text to copy it.** Release the mouse and it is on your clipboard,
  sent to your terminal over ssh as well as locally. Middle click pastes.
- **Double-click a word to select it**, and it is copied the same way a drag is:
  the highlight _is_ the copy. What counts as one word is `word_separators`,
  which lists what a word stops at rather than what it is made of, so
  `src/app.c:1234`, `https://a.b/c?d=1`, `snake_case` and a run of CJK each come
  out whole, while a quote or a bracket ends one. A click on a blank cell or on
  a separator selects nothing and leaves your clipboard as it was.
- **Images work.** This is still in it's early stages but Kitty graphics pass
  through to your terminal, including the
  half of the protocol other multiplexers drop, where a program uploads an image
  once and places it every frame. Your terminal's cell size is carried through to
  each pane, so a program can size an image without saying how many cells to
  cover. PNG uploads are decoded here, because the terminal core ships no image
  decoder. (Sixel is not supported.)
- **A pane that beeps says so**: in its title, in its tab, and until you look at
  it. `bell_indicator false` makes a bell silent _and_ invisible, which is a real
  choice. A [chrome shader](chrome.md) can make it louder.
