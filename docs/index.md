# slosh

A terminal multiplexer: panes, tabs, and sessions you can detach from, written
in C on top of [libghostty-vt](https://github.com/ghostty-org/ghostty).

## Install

### All platforms

We provide binary builds for all supported platform. Get the latest slosh release binaries from our [release](https://github.com/sl0p-foo/slosh/releases) page.

### macOS

With [Homebrew](https://brew.sh):

```bash
brew tap sl0p/slosh https://git.sl0p.foo/homebrew-slosh.git
brew install slosh
```

### Linux

#### Arch

A pacman repository serves built, signed packages. Enroll the signing key
(fingerprint `0F67C909938CFFA8A76B59FD8D69B55A0E9056B9`; the same key is
`contrib/arch/slosh.asc` in git, so the two copies can be compared):

```sh
curl -fsSLO https://slosh.foo/arch/slosh.asc
sudo pacman-key --add slosh.asc
sudo pacman-key --lsign-key 0F67C909938CFFA8A76B59FD8D69B55A0E9056B9
```

then, in `/etc/pacman.conf`:

```ini
[slosh]
Server = https://slosh.foo/arch/$arch
```

and `pacman -Sy slosh`. Details, and the PKGBUILD for building it yourself:
`contrib/arch/README.md` in the source tree.

### Source

Build from source. That needs [zig](https://ziglang.org) 0.16.

```bash
make vendor    # build the vendored terminal core (once, a couple of minutes)
make all       # a few seconds
make test      # optional: the whole suite, about thirteen seconds
sudo make install
```

## Run

```bash
slosh              # attach to session "main", creating it if needed
slosh -s work      # a named session
slosh ls           # what is running
```

Sessions keep running when you detach, or when your terminal or ssh connection
goes away. Reattach with the same command.

**TIP:** press `C-a ?` for the key combo cheatsheet, or `C-a p` for the command palette.

## Where to go

- **[Keys](keys.md)**: the leader, the defaults, rebinding, the palette.
- **[Actions](actions.md)**: every action you can bind or run from the palette.
- **[Panes and tabs](panes.md)**: splitting, moving, zooming, the mouse.
- **[Configuration](config.md)**: one file, live reload, themes, `include`.
- **[Shaders](shaders.md)**: colour passes over a pane's contents.
- **[Chrome shaders](chrome.md)**: the same passes over a pane's frame.
- **[Layouts](layouts.md)**: a session as a file you check in.
- **[Workspaces](workspaces.md)**: projects on disk, opened by name.
- **[Scripting](scripting.md)**: the control socket, and what a pane can draw.
- **[How it works](design.md)**: the two decisions that shape the rest.

About 15k lines of C, no dependencies beyond libc and the vendored core.
