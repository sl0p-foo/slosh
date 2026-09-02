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
brew tap sl0p-foo/slosh
brew install slosh
```

The explicit URL is only needed because the tap is not on GitHub; brew
remembers it after the first tap. `brew install --HEAD slosh` builds the tip of
master instead of the pinned release.

(Getting into `homebrew-core` — plain `brew install slosh`, no tap — has a
higher bar: a stable tagged release, a LICENSE, notable usage, and no
network access during the build. See "known rough edges" below.)

## Why source and not the signed binary

The obvious thought, once there is a mac builder producing a signed, notarized
zip, is to point brew at that zip instead. Three reasons not to:

- **Notarization buys nothing here.** Gatekeeper only checks binaries carrying
  the `com.apple.quarantine` attribute, and the curl that fetches a formula's
  tarball does not set it. (Casks are different: they manage quarantine
  deliberately.) Every bottle in homebrew-core is unsigned-but-for-ad-hoc and
  runs fine. The notarized zip matters for the *web* download, where the
  browser does set the attribute — not for `brew install`.
- **The build is a minute and needs one thing.** zig. Formulae that take ten
  minutes and drag in twenty dependencies are the ones that need binaries.
- **Source is the only path that covers Linux**, which brew also runs on.

If the build time ever does become the complaint, the answer is **bottles**,
not a cask and not a hand-rolled binary url. A bottle is Homebrew's own binary
format: a tarball of the installed keg, listed in a `bottle do` block with a
`root_url` pointing at wherever we host them. It is *additive* — the formula
keeps its source path, and anyone whose macOS version we have not bottled
falls back to building. The cost is a matrix: bottles are tagged per macOS
version *and* arch (`arm64_sequoia`, `arm64_sonoma`, ...), each one built on
that version. With one builder mac that means one tag covered and everyone
else compiling — which is most of the work for a fraction of the benefit,
until there are more builders.

## Releasing

`slosh.rb` here is the source of truth; the tap repo gets a copy.

**This needs no mac and no brew.** `brew-release` resolves a ref, downloads the
snapshot and hashes it, so cutting a release works from the Linux box — which
is the point, because that is where releases are cut. Signed macOS binaries are
a separate pipeline (`make macos-dist`) that brew is not involved in.

```bash
git fetch origin && git push origin v0.2.0             # brew-release pins what
                                                       # the server can serve
contrib/brew-release --tap ../homebrew-slosh          # pin origin/master
contrib/brew-release --tap ../homebrew-slosh v0.2.0   # pin a pushed tag
```

Then, on a machine that has brew (either OS), prove it before pushing the tap:

```bash
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
