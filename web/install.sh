#!/bin/sh
# slosh installer -- https://slosh.foo
#
# On macOS with Homebrew there is a shorter road:
#
#     brew tap sl0p/slosh https://git.sl0p.foo/homebrew-slosh.git
#     brew install slosh
#
# and on Arch there is a signed pacman repo -- https://slosh.foo/arch/.
#
# You are reading this before running it, which is correct. What it does:
#
#   1. works out your OS and architecture,
#   2. downloads the prebuilt static binary for it from
#      https://slosh.foo/dist/ (the newest release, or SLOSH_VERSION=),
#   3. checks it against the SHA256SUMS published next to it,
#   4. installs the one file into /usr/local/bin (the sudo is for this step
#      and nothing else).
#
# No root at all is also fine: pipe into `PREFIX=$HOME/.local sh` instead of
# `sudo sh` and the binary lands in ~/.local/bin, owned by you.
#
# Nothing is compiled, no package manager is touched, no config is written,
# nothing is started. Uninstall is `rm $PREFIX/bin/slosh`.
#
# Windows is not this script's job: slosh runs there natively as slosh.exe --
# grab the zip from https://slosh.foo/dist/ and see docs/windows.md.
#
# Prefer to build from source? It is three commands and one dependency
# (zig 0.16): clone https://slosh.foo/src, `make vendor`, `make`.

set -eu

BASE="${SLOSH_DIST:-https://slosh.foo/dist}"
PREFIX="${PREFIX:-/usr/local}"

say()  { printf '  %s\n' "$*"; }
fail() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

fetch() { # fetch URL [outfile] -- curl where it exists, wget where it doesn't
    if command -v curl >/dev/null 2>&1; then
        if [ $# -gt 1 ]; then curl -fsSL -o "$2" "$1"; else curl -fsSL "$1"; fi
    elif command -v wget >/dev/null 2>&1; then
        if [ $# -gt 1 ]; then wget -qO "$2" "$1"; else wget -qO- "$1"; fi
    else
        fail "neither curl nor wget found"
    fi
}

sha256_of() { # portable: coreutils on linux, shasum on mac, openssl anywhere
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    elif command -v openssl >/dev/null 2>&1; then openssl dgst -sha256 "$1" | sed 's/.*= *//'
    else fail "no sha256sum, shasum or openssl here -- cannot verify the download"
    fi
}

# The platform, spelled the way the release artifacts spell it.
os="$(uname -s 2>/dev/null || true)"
arch="$(uname -m 2>/dev/null || true)"
case "$os" in
    Linux)
        case "$arch" in
            x86_64|amd64)  plat=linux-x86_64;  ext=tar.gz ;;
            aarch64|arm64) plat=linux-aarch64; ext=tar.gz ;;
            *) fail "no prebuilt binary for linux/$arch yet -- building from
  source is short (zig 0.16, then make vendor && make): https://slosh.foo/src" ;;
        esac ;;
    Darwin)
        case "$arch" in
            arm64) plat=macos-arm64; ext=zip ;;
            *) fail "no prebuilt binary for intel macs yet -- use Homebrew
  (brew tap sl0p/slosh https://git.sl0p.foo/homebrew-slosh.git && brew install slosh)" ;;
        esac ;;
    *MINGW*|*MSYS*|*CYGWIN*)
        fail "Windows runs slosh.exe -- download the zip from $BASE/ (see docs/windows.md)" ;;
    *)
        fail "unrecognized platform $os/$arch -- the binaries live at $BASE/" ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

# The version: whatever SHA256SUMS says is newest for this platform, unless
# the caller pinned one. SHA256SUMS is also the checksum check below, so one
# fetch serves twice.
say "fetching $BASE/SHA256SUMS"
fetch "$BASE/SHA256SUMS" "$tmp/SHA256SUMS"
if [ -n "${SLOSH_VERSION:-}" ]; then
    ver="${SLOSH_VERSION#v}"
else
    ver="$(sed -n "s/^.*  slosh-\(.*\)-$plat\.$ext\$/\1/p" "$tmp/SHA256SUMS" \
           | sort -V | tail -1)"
    [ -n "$ver" ] || fail "no $plat build listed in $BASE/SHA256SUMS"
fi

stem="slosh-$ver-$plat"
asset="$stem.$ext"

say "downloading $BASE/$asset"
fetch "$BASE/$asset" "$tmp/$asset"

want="$(sed -n "s/^\([0-9a-f]\{64\}\)  $asset\$/\1/p" "$tmp/SHA256SUMS" | tail -1)"
[ -n "$want" ] || fail "$asset is not in SHA256SUMS -- refusing to install it"
got="$(sha256_of "$tmp/$asset")"
[ "$got" = "$want" ] || fail "checksum mismatch for $asset
  expected $want
  got      $got"
say "sha256 verified"

case "$ext" in
    tar.gz) tar -xzf "$tmp/$asset" -C "$tmp" ;;
    zip)    unzip -q "$tmp/$asset" -d "$tmp" ;;
esac
[ -f "$tmp/$stem/slosh" ] || fail "$asset did not contain $stem/slosh"

say "installing to $PREFIX/bin/slosh"
install -d "$PREFIX/bin"
install -m 755 "$tmp/$stem/slosh" "$PREFIX/bin/slosh"

# The one way a non-root install goes quietly wrong: ~/.local/bin exists now
# but the shell does not look there. Say so instead of letting `slosh` print
# "command not found" a minute after "done".
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) say "note: $PREFIX/bin is not in your PATH -- add it, e.g.:"
       say '      export PATH="'"$PREFIX/bin"':$PATH"' ;;
esac

say "done. run: slosh"
