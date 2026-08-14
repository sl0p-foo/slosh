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
it and shows a dashed line where the split would land. Click a pane to focus
it, a tab to switch, `+tab` to open one, a collapsed header to expand it, and a
pane's own buttons to talk to the program inside it. **Drag a pane's title bar
onto another pane to swap them, or drag the gap between two panes to move the
boundary.** While you drag, every *other* pane drains to grey and drops in
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

**Shaders** are colour passes over a pane's *contents* — never the frame, the
title or the tab strip, so chrome stays legible over a pane that has been
dimmed underneath it. `dim_unfocused` (off by default) fades every pane but
the focused one; `drag_grayscale` and `drag_dim` are the greying and the
recession you get while dragging. All are strengths from 0 to 255. Because a cell's colour is often "whatever the
terminal calls default", shaders resolve those through `theme`'s `default_fg`
and `default_bg` — set them to your terminal's real colours, or a shaded pane
will shift as it turns on. Panes with no shader defer to your terminal exactly
as before.

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
