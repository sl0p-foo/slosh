# sl0ppty

A terminal multiplexer — panes, tabs, sessions you can detach from — written in
C on top of [libghostty-vt](https://github.com/ghostty-org/ghostty).

```
╭──────────── nvim ─────────── ▬ □ ✕ ╮  ╭──────── npm run dev ──────── ▬ □ ✕ ╮
│                                     │  │                                    │
│                                     │  ╰────────────────────────────────────╯
│                                     │  ╭──────────── shell ───────── ▬ □ ✕ ╮
│                                     │  │                                    │
╰─────────────────────────────────────╯  ╰────────────────────────────────────╯
```

## Install

Needs [zig](https://ziglang.org) 0.16 and nothing else. The result is one static
binary.

```bash
make vendor    # the vendored terminal core, once
make           # about a second
```

## Run

```bash
sl0ppty              # attach to session "main", creating it if needed
sl0ppty -s work      # a named session
sl0ppty ls           # what is running
```

Sessions survive detaching, and your ssh connection dying. Then `C-a \` to
split, `C-a ?` for the keys, `C-a p` for every command by name.

## Docs

**[docs/](docs/index.md)** — [keys](docs/keys.md) ·
[panes](docs/panes.md) · [configuration](docs/config.md) ·
[shaders](docs/shaders.md) · [chrome](docs/chrome.md) ·
[layouts](docs/layouts.md) · [scripting](docs/scripting.md) ·
[how it works](docs/design.md)

## What is different about it

- **The mouse works properly.** Click a border to split toward it, drag a gap to
  move a boundary, drag a pane by its title to swap it. Everything drawn
  registers what it is as it is drawn, so a click and a preview cannot disagree.
- **The layout is recomputed, never stored.** Small terminal? The tab becomes a
  list of headers, and comes back to exactly the arrangement you had.
- **A pane told to run something keeps its output when that thing exits**, with
  `[re-run]` and `[close]` in its frame.
- **Configuration is one KDL file** that reloads on save, can be built from
  `include`d pieces, and refuses a broken file instead of half-applying it.
- **Colour passes over cells** — dim, tint, rulers, spotlights, and a border that
  can flash when a pane rings, with strengths written as expressions.
- **Scriptable to the same depth it is usable**: one JSON object per line over
  the session's socket, and a pane can draw its own status bar and buttons with
  an escape sequence.

Design notes and the twenty decisions behind it: [DESIGN.md](DESIGN.md).
