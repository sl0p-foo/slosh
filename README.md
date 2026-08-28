# slosh

A terminal multiplexer: panes, tabs, and sessions you can detach from, written
in C on top of [libghostty-vt](https://github.com/ghostty-org/ghostty).

## Install

### macOS

With [Homebrew](https://brew.sh):

```bash
brew tap sl0p/slosh https://git.sl0p.foo/homebrew-slosh.git
brew install slosh
```

### Linux

The same tap works under [Homebrew on
Linux](https://docs.brew.sh/Homebrew-on-Linux). Building from source needs
[zig](https://ziglang.org) 0.16 and nothing else, and the result is one static
binary that runs on any distro:

```bash
make vendor    # the vendored terminal core, once
make all       # about a second
sudo make install         # optional: /usr/local/bin/slosh
# make install PREFIX=$HOME/.local   # ...or somewhere you own
```

(The source build works on a mac too; Homebrew is simply the shorter road
there.)

### Windows

Windows 10/11, x86-64 and ARM64, as one `slosh.exe` with no runtime
dependency beyond the OS. Panes are ConPTYs and sessions are AF_UNIX sockets,
so everything below works there too — see [docs/windows.md](docs/windows.md).

Cross-compiled from a mac or a Linux box, then copied across:

```bash
make -f Makefile.windows ARCH=x86_64    # or ARCH=aarch64
```

The binary lands at `build/win-<arch>/slosh.exe`; the `dist` target zips it
with its licensing paperwork, and `make release` builds both architectures
alongside the Linux and macOS artifacts.

## Run

```bash
slosh              # attach to session "main", creating it if needed
slosh -s work      # a named session
slosh ls           # what is running
```

Sessions survive detaching, and your ssh connection dying. Then `C-a \` to
split, `C-a ?` for the keys, `C-a p` for every command by name.

## Docs

**[docs/](docs/index.md)**: [keys](docs/keys.md) ·
[panes](docs/panes.md) · [configuration](docs/config.md) ·
[windows](docs/windows.md) ·
[shaders](docs/shaders.md) · [chrome](docs/chrome.md) ·
[layouts](docs/layouts.md) · [scripting](docs/scripting.md) ·
[how it works](docs/design.md)

`make docs` renders the same pages as a static site into `build/docs`, with no
dependencies: one file of Python and one stylesheet.

## What is different about it

- **The mouse works properly.** Click the middle of a border to split toward it,
  drag a gap to move a boundary, drag a pane by its title to swap it. Everything
  drawn registers what it is as it is drawn, so a click and a preview cannot
  disagree. That is also how the split target can be *part* of an edge without
  the highlight and the hit ever drifting apart.
- **The layout is recomputed, never stored.** Small terminal? The tab becomes a
  list of headers, and comes back to exactly the arrangement you had.
- **A pane told to run something keeps its output when that thing exits**, with
  `[re-run]` and `[close]` in its frame.
- **Configuration is one KDL file** that reloads on save, can be built from
  `include`d pieces, and refuses a broken file instead of half-applying it.
- **Colour passes over cells**: dim, tint, rulers, spotlights, and a border that
  can flash when a pane rings, with strengths written as expressions.
- **Scriptable to the same depth it is usable**: one JSON object per line over
  the session's socket, and a pane can draw its own status bar and buttons with
  an escape sequence.

Design notes and the twenty-one decisions behind it: [DESIGN.md](DESIGN.md).
