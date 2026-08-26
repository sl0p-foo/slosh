# Homebrew

`slosh.rb` is a **formula**, which is the right kind of thing. The distinction,
since it is the first question anyone asks:

| | what it is for | what it does |
|---|---|---|
| **formula** | command-line software | brew builds it (or fetches a prebuilt "bottle") into `Cellar/` and symlinks `bin/` |
| **cask** | prebuilt macOS apps: `.app`, `.dmg`, `.pkg` | brew unpacks the drop into `/Applications`, builds nothing |

slosh is a binary you run in a terminal and it builds from source in under a
minute, so: formula. A cask would be the answer only if we shipped the signed,
notarized zip that `Makefile.macos` produces and wanted brew to just drop it in
place.

## The tap

A formula has to live in a *tap* — a git repo named `homebrew-<tapname>` full
of `Formula/*.rb`. Ours is `homebrew-slosh`, so users say:

```bash
brew tap sl0p/slosh https://git.sl0p.foo/homebrew-slosh.git
brew install slosh
```

The explicit URL is only needed because the tap is not on GitHub; brew
remembers it after the first tap. `brew install --HEAD slosh` builds the tip of
master instead of the pinned release.

(Getting into `homebrew-core` — plain `brew install slosh`, no tap — has a
higher bar: a stable tagged release, a LICENSE, notable usage, and no
network access during the build. See "known rough edges" below.)

## Releasing

`slosh.rb` here is the source of truth; the tap repo gets a copy.

```bash
contrib/brew-release --tap ../homebrew-slosh          # pin origin/master
contrib/brew-release --tap ../homebrew-slosh v0.2.0   # pin a pushed tag
brew install --build-from-source contrib/homebrew/slosh.rb
brew test slosh
```

The script resolves the ref to a full commit id, downloads the cgit snapshot,
hashes it, and rewrites `url` / `version` / `sha256`. Untagged pins are
versioned `0.1.0-<commits>-<short>` so brew can tell which build is newer;
a tag is used verbatim.

Before pushing a formula change:

```bash
brew style sl0p/slosh
brew audit --strict --tap=sl0p/slosh
```

## Known rough edges

- **The build fetches.** `zig build` downloads the pinned libghostty-vt
  dependencies from `deps.files.ghostty.org` during `make vendor`. Homebrew's
  sandbox allows this (verified), but homebrew-core forbids it — that would
  need the zig packages vendored into the tarball or declared as `resource`s.
- **No LICENSE.** The formula therefore has no `license` stanza. Taps do not
  care; homebrew-core would refuse.
- **Snapshot tarballs.** cgit generates them on demand; the bytes have been
  stable across requests and days for a given ref, which is what the `sha256`
  relies on. If that ever changes, switch the formula to
  `url "https://git.sl0p.foo/slosh.git", using: :git, revision: "<sha>"`,
  which is content-addressed and needs no hash.
