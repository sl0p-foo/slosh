# sl0ppty

A terminal multiplexer — panes, tabs, sessions you can detach from — written
in C on top of [libghostty-vt](https://github.com/ghostty-org/ghostty).

## Why another one

The multiplexers we have are crusty, and the things that make them awkward are
not features somebody forgot to add — they follow from how the programs are
built. Layout state that is stored and edited rather than recomputed, so it
drifts out of sync with itself. Mouse handling written as a second
implementation of the drawing code, so the two disagree about where a button
is. Extension models bolted onto a core that never expected one. This project
started as a fork of one of them, carrying sixty-odd patches; almost none of
that work was *multiplexing*, it was arguing with the architecture.

So: start over, and stand on the shoulders of the person who already solved
the genuinely hard part. Mitchell Hashimoto's terminal emulation core from
[ghostty](https://ghostty.org) is vendored as a library and does the VT
parsing, scrollback, selection, images and key encoding. What sits on top is
small, recomputes rather than remembers, and is scriptable to the same depth
it is usable.

## Install

Needs [zig](https://ziglang.org) 0.16 and nothing else — no cmake, no
autotools, no libraries. The result is one static binary.

```bash
make vendor    # build the vendored terminal core (once, a couple of minutes)
make           # about a second
make test      # optional: the full suite, about six seconds
```

## Use

```bash
sl0ppty              # attach to session "main", creating it if needed
sl0ppty -s work      # a named session
sl0ppty ls           # what is running
```

Sessions keep running when you detach, or when your terminal or ssh
connection goes away. Reattach with the same command.

| key | |
|---|---|
| `C-a \` / `C-a -` | split into columns / rows |
| `C-a h j k l` or arrows | move focus |
| `C-a H J K L` or shift+arrows | move the boundary between panes |
| `C-a z` / `C-a m` | zoom a pane to fill the tab / minimise it |
| `C-a c` · `C-a n` `C-a p` · `C-a 1..9` | new tab · cycle · select |
| `C-a f` | find a pane by name |
| `C-a PgUp` `C-a PgDn` `C-a Home` `C-a End` | scrollback (the wheel works too) |
| `C-a x` / `C-a r` | close a pane / re-run a finished one |
| `C-a d` / `C-a q` | detach / quit |
| `C-a C-a` | send a literal `C-a` |

**`C-a` is a default, not a decision.** If you want it back for start-of-line —
and plenty of people do — take one line of config and it is yours; everything
that names the prefix, the badge in the status bar and the cheatsheet
included, follows what you bound:

```kdl
keys { prefix "ctrl+b" }        // or ctrl+space, or alt+x, or ...
```

### The mouse works properly

- **Click a pane's border to split toward it** — the side you click is the side
  the new pane appears on. Hovering shows a dashed line where the split would
  land.
- **Drag the gap between panes** to move the boundary; where two gaps cross,
  drag the crossing to move both at once.
- **Drag a pane by its title** onto another to swap them. Everything it could
  land on greys out; the pane in your hand does not.
- **Double-click a name to rename it, in place** — a pane's title, or a tab in
  the strip. Enter keeps it, Escape abandons it, empty gives it back.
- **Drag a tab along the strip** to reorder it; the strip rearranges as you go
  rather than dropping it somewhere at the end.
- Click a tab to switch, `+` to open one, a pane to focus it.
- Hovering anything says what it does, in a word at the bottom of the screen.

### Panes do not vanish when a command ends

When the program in a pane exits, the pane stays — with everything it printed
still on screen, a line saying how it ended, and two buttons:

```
╰ exited: status 3 ───────────────────────────────[re-run]─[close]─╯
```

`[re-run]` runs the same command again in the same pane, keeping the previous
run above it in the scrollback. So a command that failed while you were
looking somewhere else still has its error message when you get back, and a
mistyped command in a fresh session no longer closes the session.

### The rest of it

- **Select text to copy it.** Release the mouse and it is on your clipboard,
  sent to your terminal over ssh as well as locally. Middle click pastes.
- **Images work.** Kitty graphics pass through to your terminal, which is the
  thing other multiplexers drop — including the useful half of the protocol,
  where a program uploads an image once and then places it every frame. Your
  terminal's cell size is carried through to each pane, so programs can size
  images and place them without saying how many cells to cover. (Sixel is not
  supported: the terminal core has no sixel decoder.)
- **Small terminals degrade gracefully.** When panes no longer fit, the tab
  becomes a list of one-line headers with the focused pane open below them —
  and returns to exactly the layout you had when there is room again.
- **A pane that beeps says so**, in its title and in its tab, until you look.
- **Minimised panes** sit in a row along the bottom, still running.

## Configuration

[`config/config.kdl`](config/config.kdl) is the complete list of settings and
also the defaults; copy it to `~/.config/sl0ppty/config.kdl` and edit. **Saving
it applies it to every running session immediately** — and a file with a
mistake in it is refused rather than half-applied.

Geometry, key bindings and every colour are configurable. Six ready-made
themes are in [`contrib/themes`](contrib/themes).

Panes are tinted by *state*, and out of the box only for states that mean
**this pane is not live**: its program exited, it never started, or you are
looking at scrollback rather than the present. None of those is discoverable
by looking unless something says so. A pane that is merely not the one you are
in is left alone — that is a taste, and it is one line away if it is yours:

```kdl
states {
    dead { grayscale amount=200; dim amount=90 }   // shipped
    suspended { grayscale amount=170; dim amount=60 }
    scrolled { tint amount=22 color="#ff5fd7" }
    unfocused { dim amount=90 }                    // yours, if you want it
}
```

The same mechanism draws a column ruler, a margin marker or a spotlight that
follows the cursor, inside programs that have never heard of such things.

**A shader's strength can be an expression**, so an effect nobody built in
takes one line of config and no compiler:

```kdl
shaders {
    dim  amount="(y % 2) * 40"                        // scanlines
    dim  amount="(x > cols - 10) * 120"               // a right margin
    tint amount="255 - dist(x, y, curx, cury) * 12" color="#ff5fd7"
}
```

Integer arithmetic over `x y cols rows curx cury focused t`, with `min max abs
clamp dist` and comparisons that give 0 or 1 — so `(x < 10) * 200` is how you
write a rule. Thirty-two ready-made ones are in
[`contrib/shaders`](contrib/shaders) — a cursor line, a crosshair, a torch, a
phosphor CRT, sonar pings that follow your cursor — and `contrib/shader-tour`
cycles a live session through them. There are no loops, so a config cannot hang a session. The
expression produces the *strength* and never the colour, which keeps the
mixing in C and means the whole program can be worked out once into a per-cell
map and reused: a shader you wrote in your config costs about what a compiled
one does.

**You can add your own** without rebuilding sl0ppty: a shader is a C function
from one cell to that cell's colours, and any `*.so` in
`~/.config/sl0ppty/shaders/` that exports one is loaded at startup and named
in the config exactly like a built-in. Skeleton, Makefile and the rules a
shader has to keep: [`contrib/shader-plugin`](contrib/shader-plugin). It is
native code in the session's process, so install ones you trust — the same
standing as `shell` and a layout's `command=`, which can already run anything
as you.

## Scripting

Three things make sl0ppty easy to drive from other programs.

**A pane can draw its own status bar and buttons**, by printing an escape
sequence — no plugin, no config:

```bash
printf '\033]5577;1;status;building 3/7\033\\'
printf '\033]5577;1;buttons;approve:Approve;cancel:Cancel\033\\'
# clicking [Approve] arrives on the program's stdin as:
#   \033]5577;1;click;approve\033\\
```

**A session can be a file**, so a project's window layout is checked in with
the project:

```kdl
layout {
    tab name="api" cwd="~/dev/api" {
        pane command="nvim"
        pane split="rows" {
            pane command="npm run dev" suspended=true
            pane
        }
    }
}
```

```bash
sl0ppty --layout session.kdl
```

`suspended` panes are laid out but run nothing until you touch them, so twelve
checked-out projects are not twelve running dev servers. Panes and tabs can
also carry a `purpose=` label for tooling to find them by, which a program
inside a pane cannot overwrite. Full example:
[`config/layout.example.kdl`](config/layout.example.kdl).

**Everything the keyboard can do, a script can do**, over the session's socket
— one JSON object per line:

```bash
$ sl0ppty -s work cmd '{"cmd":"new-tab","name":"api"}'
{"ok":true,"id":2}
$ sl0ppty -s work cmd '{"cmd":"panes"}'
{"ok":true,"panes":[{"id":1,"title":"nvim","alive":true,...}]}
```

Panes are addressed by id, so a background tab is scriptable, and a detached
session answers exactly as an attached one does.

## Under the hood

Two decisions do most of the work, and [DESIGN.md](DESIGN.md) explains the
rest:

**Everything drawn registers what it is** as it is drawn, and a click is a
lookup in that list. Drawing and clicking cannot disagree, because there is
only one of them.

**It runs headless.** `sl0ppty --script` is the whole thing without a terminal:
commands in, the composited screen out as JSON. That is how it is tested —
drive these events, assert this screen — so the suite is 700-odd real
end-to-end checks that finish in six seconds.

About 10k lines of C, no dependencies beyond libc and the vendored terminal
core.
