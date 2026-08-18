# slosh

A terminal multiplexer — panes, tabs, sessions you can detach from — written in
C on top of [libghostty-vt](https://github.com/ghostty-org/ghostty).

```
╭────────── nvim ─────────── ▬ □ ✕ ╮  ╭─────── npm run dev ─────── ▬ □ ✕ ╮
│                                  │  │                                  │
│                                  │  │                                  │
│                                  │  │                                  │
│                                  │  │                                  │
│                                  │  ╰──────────────────────────────────╯
│                                  │
│                                  │  ╭────────── shell ────────── ▬ □ ✕ ╮
│                                  │  │                                  │
│                                  │  │                                  │
│                                  │  │                                  │
│                                  │  │                                  │
╰──────────────────────────────────╯  ╰──────────────────────────────────╯
```

## Install

Needs [zig](https://ziglang.org) 0.16 and nothing else — no cmake, no
autotools, no libraries. The result is one static binary.

```bash
make vendor    # build the vendored terminal core (once, a couple of minutes)
make all       # a few seconds
make test      # optional: the whole suite, about thirteen seconds
```

## Run

```bash
slosh              # attach to session "main", creating it if needed
slosh -s work      # a named session
slosh ls           # what is running
```

Sessions keep running when you detach, or when your terminal or ssh connection
goes away. Reattach with the same command.

Then: `C-a \` to split, `C-a ?` for the cheatsheet, `C-a p` for every command
by name.

## Where to go

- **[Keys](keys.md)** — the leader, the defaults, rebinding, the palette.
- **[Panes and tabs](panes.md)** — splitting, moving, zooming, the mouse.
- **[Configuration](config.md)** — one file, live reload, themes, `include`.
- **[Shaders](shaders.md)** — colour passes over a pane's contents.
- **[Chrome shaders](chrome.md)** — the same passes over a pane's frame.
- **[Layouts](layouts.md)** — a session as a file you check in.
- **[Scripting](scripting.md)** — the control socket, and what a pane can draw.
- **[How it works](design.md)** — the two decisions that shape the rest.

## Why another one

The multiplexers we have are crusty, and the things that make them awkward are
not features somebody forgot to add — they follow from how the programs are
built. Layout state that is stored and edited rather than recomputed, so it
drifts out of sync with itself. Mouse handling written as a second
implementation of the drawing code, so the two disagree about where a button
is. Extension models bolted onto a core that never expected one.

So: start over, and stand on the shoulders of the person who already solved the
genuinely hard part. The terminal emulation core from
[ghostty](https://ghostty.org) is vendored as a library and does the VT
parsing, scrollback, selection, images and key encoding. What sits on top is
small, recomputes rather than remembers, and is scriptable to the same depth it
is usable.

About 15k lines of C, no dependencies beyond libc and the vendored core.
