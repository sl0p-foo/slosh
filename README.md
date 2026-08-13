# sl0ptty

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
sl0ptty                  # attach to session "main", creating it if needed
sl0ptty -s work          # a named session
sl0ptty ls               # live sessions
sl0ptty cmd '{"cmd":"panes"}'
```

| key | |
|---|---|
| `C-a \` / `C-a -` | split into columns / rows |
| `C-a h j k l` or arrows | move focus |
| `C-a c` / `C-a n` `C-a p` / `C-a 1..9` | new tab / cycle / select |
| `C-a f` | find a pane by title, purpose or tab |
| `C-a x` / `C-a d` / `C-a q` | close pane / detach / quit |
| `C-a C-a` | send a literal `C-a` |

The mouse works throughout: click a pane to focus it, a tab to switch, `+` on a
frame to split, `+tab` to open one, a collapsed header to expand it, and a
pane's own buttons to talk to the program inside it.

Configuration is [`config/config.kdl`](config/config.kdl), which documents
every setting by being the defaults. Copy it to `~/.config/sl0ptty/config.kdl`.

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

## Control API

One JSON object per line, on the session's unix socket. It is the same
vocabulary the test harness drives, so nothing tested can drift from what the
API does.

```bash
$ sl0ptty -s work cmd '{"cmd":"new-tab","name":"api","purpose":"project:api.a1b2"}'
{"ok":true,"id":2}
$ sl0ptty -s work cmd '{"cmd":"panes"}'
{"ok":true,"panes":[{"id":1,"x":2,"y":2,...,"purpose":"agent:main"}]}
```

`panes tabs snapshot send raw resize split focus close new-tab select-tab
set-name set-purpose reload alive quit`. Panes are addressed by id, so a
background tab is scriptable, and a detached session answers exactly as a live
one does.

## How it is built

See [DESIGN.md](DESIGN.md) for the reasoning and the decisions. The two that
shape everything:

**One geometry.** Every painted interactive element records its `(rect,
action)` as it is painted, and a click is a lookup in that list. Drawing and
hit-testing cannot disagree, because there is only one of them.

**Headless from day one.** `sl0ptty --script` runs the whole compositor with no
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
