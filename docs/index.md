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
Linux](https://docs.brew.sh/Homebrew-on-Linux), or build from source. That
needs [zig](https://ziglang.org) 0.16 and nothing else: no cmake, no
autotools, no libraries. The result is one static binary that runs on any
distro.

```bash
make vendor    # build the vendored terminal core (once, a couple of minutes)
make all       # a few seconds
make test      # optional: the whole suite, about thirteen seconds
sudo make install
```

(The source build works on a mac too; Homebrew is simply the shorter road
there.)

### Windows

Windows 10/11, x86-64 and ARM64, as one `slosh.exe`. Panes are ConPTYs and
sessions are AF_UNIX sockets, so everything documented here works there too.

```bash
make -f Makefile.windows ARCH=x86_64    # or ARCH=aarch64; cross-compiles too
```

The details, and the three decisions the port rests on: [windows](windows.md).

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

- **[Keys](keys.md)**: the leader, the defaults, rebinding, the palette.
- **[Panes and tabs](panes.md)**: splitting, moving, zooming, the mouse.
- **[Configuration](config.md)**: one file, live reload, themes, `include`.
- **[Shaders](shaders.md)**: colour passes over a pane's contents.
- **[Chrome shaders](chrome.md)**: the same passes over a pane's frame.
- **[Layouts](layouts.md)**: a session as a file you check in.
- **[Workspaces](workspaces.md)**: projects on disk, opened by name.
- **[Scripting](scripting.md)**: the control socket, and what a pane can draw.
- **[How it works](design.md)**: the two decisions that shape the rest.

About 15k lines of C, no dependencies beyond libc and the vendored core.
