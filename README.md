# sl0ppty

An opinionated terminal multiplexer in C, built on
[libghostty-vt](https://github.com/ghostty-org/ghostty).

It exists because driving LLM agent sessions wants a multiplexer whose *chrome
is programmable* — a pane can draw its own status line and clickable buttons —
and bending an existing one into that shape cost more than writing this did.

```
   1  2:build  +tab                                        5 panes
   pi · ready────────────────────────────────────────────────────
   pi · ready────────────────────────────────────────────────────
  ╭──────────────────────────── pi ──────────────────────────+─╮
  │                                                            │
  │                                                            │
  ╰ alt+N · ready · #7 ────────────────────[continue]─[explain]─╯
```

The bottom row is the pane's own: it printed it with an escape sequence, and a
click on `[continue]` is delivered to it on stdin.

## Build

Needs [zig](https://ziglang.org) 0.16 (for libghostty-vt, and as the C
compiler) and nothing else — no cmake, no autotools, no libraries.

```bash
make vendor    # build the vendored libghostty-vt (once, ~2 min)
make           # 0.15s
make test      # 250 checks, ~30s
make test-live # the ones that need a real tty
```

## Use

```bash
sl0ppty                  # attach to session "main", creating it if needed
sl0ppty -s work          # a named session
sl0ppty ls               # live sessions
sl0ppty cmd '{"cmd":"panes"}'
```

| key | |
|---|---|
| `C-a \` / `C-a -` | split into columns / rows |
| `C-a h j k l` or arrows | move focus |
| `C-a c` / `C-a n` `C-a p` / `C-a 1..9` | new tab / cycle / select |
| `C-a f` | find a pane by title, purpose or tab |
| `C-a H J K L` or shift+arrows | move the boundary (the pane grows or shrinks accordingly) |
| `C-a PgUp` `C-a PgDn` `C-a Home` `C-a End` | scrollback (the wheel does it too) |
| `C-a x` / `C-a d` / `C-a q` | close pane / detach / quit |
| `C-a C-a` | send a literal `C-a` |

The mouse works throughout. **Click a pane's border to split toward it** — the
side you click is the side the new pane appears on, and hovering a border arms
it and shows a dashed line where the split would land, with an arrow (`◄ ► ▲
▼`) on that line pointing at the half the new pane will take. Below
`min_split` the border stops being a button entirely: no guide, no split, on
the reasoning that two twenty-five column panes technically fit and are not
worth having. Click a pane to focus
it, a tab to switch, `+tab` to open one, a collapsed header to expand it, and a
pane's own buttons to talk to the program inside it. **Drag a pane's title bar
onto another pane to swap them, or drag the gap between two panes to move the
boundary.** Resting on a gap marks it dotted to say it is a handle, with an
arrow in the middle for the way it travels, and doubles the mark while you are
actually moving it. While you drag a pane,
every *other* pane drains to grey and drops in
brightness while its border turns dashed — they are all places the pane could
land — and the one in your hand keeps its colour and a solid border, so it
lifts off the page. Focus follows the pointer (`focus_follows_mouse`, on by
default, and it knows when not to).

**Double-click a pane's name to rename it**, in the title itself rather than in
a dialog. Enter keeps the new name, Escape abandons it, clicking away keeps it.
A name you type outranks whatever the program calls itself, so a shell that
rewrites its title cannot undo it; clear the name and the pane goes back to the
program's title. The name is only the title *text*: the rest of the top border
is still the top edge, and still splits upward when clicked.

**Select to copy.** Drag over text and release: it is on your clipboard, sent
to your terminal as OSC 52 so it works over ssh. Middle click pastes it back,
the way a primary selection behaves everywhere else. A program in a pane can
copy the same way (OSC 52) and can raise a notification (OSC 9), which appears
as a toast in the corner — as do `{"cmd":"notify","text":"..."}` and things
like "copied 13 chars".

A **BEL** from a program marks the pane it came from and the tab that pane is
in, until you look at it. A bell is for the pane you are *not* watching, so one
that only showed on screen would be useless in a background tab. `bell_indicator
false` turns it off; `bell_mark` chooses the character.

**Shaders** are colour passes over a pane's *contents* — never the frame, the
title or the tab strip, so chrome stays legible over a pane that has been
dimmed underneath it. A `states { }` block says what a pane looks like while
it is in a state — being dragged, somewhere a drag could land, suspended,
scrolled back, or simply not focused. A pane is usually in several at once, so
exactly one wins, the most deliberate rather than the most ambient:

```kdl
states {
    dragging { }                                    // untouched, in your hand
    drop_target { grayscale amount=200; dim amount=140 }
    suspended { grayscale amount=180; dim amount=90 }
    scrolled { tint amount=30 color="#ffcc88" }     // you are in the past
    unfocused { dim amount=90 }
}
```

A `shaders { }` block is the same thing every pane gets regardless, in the
order you write it:

```kdl
shaders {
    vignette amount=70                        // darken towards the edges
    zebra amount=22                           // band alternate rows
    ruler amount=60 at=80 color="#ff5fd7"     // a column guide, in any program
    margin amount=90 at=100                   // long lines run out of the pane
    spotlight amount=90 radius=14             // brightness follows the cursor
    gradient amount=40 direction=0            // fade down the pane
}
```

Each is a pure function of a cell's colour and where it sits, which is why a
ruler costs no columns and works inside a program that has never heard of one. Because a cell's colour is often "whatever the
terminal calls default", shaders resolve those through `theme`'s `default_fg`
and `default_bg` — set them to your terminal's real colours, or a shaded pane
will shift as it turns on. Panes with no shader defer to your terminal exactly
as before.

A **strip along the top** carries the tabs, the pane count and the prefix
indicator; a **line along the bottom** says what you are looking at — session,
tab, the focused pane's purpose or title, which of the tab's panes it is
(`pane 2/5`), and whether it is scrolled back, on an alternate screen, or not
started yet. Either can be turned off
(`status_bar`, `status_line`), and the panes get the row back.

Every colour the compositor draws has its own name in `theme` — the split
guide, the resize handle, a drop target, the scroll indicator, the rows of a
collapsed tab, each state of a tab, the status line, the finder, toasts, the
rename editor. Several share a default, which says they look right together
rather than that they are the same thing.

Configuration is [`config/config.kdl`](config/config.kdl), which documents
every setting by being the defaults. Copy it to `~/.config/sl0ppty/config.kdl`.

## Panes can draw their own chrome (OSC 5577)

```bash
printf '\033]5577;1;status;building 3/7\033\\'
printf '\033]5577;1;buttons;approve:Approve;cancel:Cancel\033\\'
# a click arrives on stdin as:  \033]5577;1;click;approve\033\\
printf '\033]5577;1;clear\033\\'
```

Byte-compatible with the [sl0ppi](https://sl0ppi.sl0p.foo) zellij fork, so
extensions written against that work here unmodified. A pane can also propose
its own `purpose` (`\033]5577;1;purpose;agent:main\033\\`) — but a purpose
declared by a layout outranks it and cannot be overridden, so a pane cannot
relabel itself into something tooling trusts.

## Images

Kitty graphics pass through. A program in a pane transmits an image and it
appears, because libghostty-vt tracks the images and placements and sl0ppty
re-emits them to your terminal with ids remapped so two panes using image `7`
stay two images. Placements follow the layout, are cropped at the pane's edges
by moving the source rectangle (asking for fewer cells would scale instead),
and are deleted when their pane closes or scrolls away.

This is the thing tmux and zellij drop, and the reason tools fall back to
sixel under them. Sixel itself is *not* supported: libghostty-vt has no sixel
decoder, so those sequences are swallowed.

## Sessions as files

```kdl
layout {
    tab name="api" purpose="project:api.a1b2c3" cwd="~/dev/api" {
        pane purpose="agent:main" command="pi"
        pane split="rows" {
            pane purpose="service:web" command="npm run dev" suspended=true
            pane purpose="shell:scratch"
        }
    }
}
```

```bash
sl0ppty --layout session.kdl                       # build it
sl0ppty cmd '{"cmd":"apply-layout","path":"..."}'  # add it to a live session
```

`suspended` panes exist, are laid out, and have run nothing — they start on
their first keystroke, because twelve projects should not be twelve running
dev servers. Purposes a layout declares are locked, so identity comes from the
file rather than from whatever the program inside prints. Full example with
commentary: [`config/layout.example.kdl`](config/layout.example.kdl).

## Control API

One JSON object per line, on the session's unix socket. It is the same
vocabulary the test harness drives, so nothing tested can drift from what the
API does.

```bash
$ sl0ppty -s work cmd '{"cmd":"new-tab","name":"api","purpose":"project:api.a1b2"}'
{"ok":true,"id":2}
$ sl0ppty -s work cmd '{"cmd":"panes"}'
{"ok":true,"panes":[{"id":1,"x":2,"y":2,...,"purpose":"agent:main"}]}
```

`panes tabs snapshot send raw resize split focus close new-tab close-tab
select-tab set-name set-purpose apply-layout reload alive quit`. Panes are addressed by id, so a
background tab is scriptable, and a detached session answers exactly as a live
one does.

## How it is built

See [DESIGN.md](DESIGN.md) for the reasoning and the decisions. The two that
shape everything:

**One geometry.** Every painted interactive element records its `(rect,
action)` as it is painted, and a click is a lookup in that list. Drawing and
hit-testing cannot disagree, because there is only one of them.

**Headless from day one.** `sl0ppty --script` runs the whole compositor with no
tty: commands in, composited screen out as JSON — text, style runs, cursor,
hit list. Tests are "drive these events, assert this screen", and they take
milliseconds.

```
src/
  input.c      outer terminal bytes -> semantic events (kitty, mouse, paste)
  pane.c       a pty + a libghostty-vt terminal + its OSC 5577 chrome
  app.c        the layout tree, tabs, focus, chrome, what a key does
  screen.c     the compositor: cell buffer, diff, minimal ANSI out
  server.c     the session that outlives its client
  client.c     a terminal in raw mode and a socket
  cmd.c        one command vocabulary, shared by the driver and the socket
  kdl.c        the config parser
```

~5500 lines, no dependencies beyond libc and libghostty-vt, static binary.
