#!/bin/sh
# slosh installer -- https://slosh.foo
#
# On macOS with Homebrew there is a shorter road:
#
#     brew tap sl0p/slosh https://git.sl0p.foo/homebrew-slosh.git
#     brew install slosh
#
# You are reading this before running it, which is correct. What it does:
#
#   1. checks for zig 0.16 and git,
#   2. clones the source into a temporary directory,
#   3. `make vendor && make` -- the vendored terminal core, then one static
#      binary, no other dependencies,
#   4. installs build/slosh into /usr/local/bin (the sudo is for this step
#      and nothing else).
#
# Nothing is downloaded except the git clone and what `make vendor` fetches
# (the pinned libghostty-vt). No package manager, no config written, nothing
# started. Uninstall is `rm /usr/local/bin/slosh`.
#
# Windows is not this script's job: slosh runs there natively, but as a
# cross-compiled slosh.exe rather than a shell-script install -- see
# docs/windows.md in the repo.

set -eu

REPO="${SLOSH_REPO:-https://slosh.foo/src}"   # TODO: pin to the public repo URL
PREFIX="${PREFIX:-/usr/local}"
ZIG="${ZIG:-zig}"

say()  { printf '  %s\n' "$*"; }
fail() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

case "$(uname -s 2>/dev/null || true)" in
    *MINGW*|*MSYS*|*CYGWIN*)
        fail "this builds the POSIX binary; Windows runs slosh.exe -- see docs/windows.md" ;;
esac

command -v git >/dev/null 2>&1 || fail "git is required"

# zig 0.16, the one thing the build wants. Also try the location the
# Makefile's own docs suggest.
if ! command -v "$ZIG" >/dev/null 2>&1; then
    if [ -x "$HOME/zig-0.16.0/zig" ]; then
        ZIG="$HOME/zig-0.16.0/zig"
    else
        fail "zig 0.16 not found -- https://ziglang.org/download, or ZIG=/path/to/zig"
    fi
fi
case "$("$ZIG" version 2>/dev/null)" in
    0.16.*) ;;
    *) fail "zig 0.16 is required, found $("$ZIG" version 2>/dev/null || echo nothing)" ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

say "cloning $REPO"
git clone --quiet --depth 1 "$REPO" "$tmp/slosh"

say "building (make vendor, once; then make all)"
make -C "$tmp/slosh" ZIG="$ZIG" vendor
# `all`, spelled out: the default goal is the help text, so a bare `make` here
# would print it cheerfully and install whatever build/slosh happened to exist.
make -C "$tmp/slosh" ZIG="$ZIG" all

say "installing to $PREFIX/bin/slosh"
install -d "$PREFIX/bin"
install -m 755 "$tmp/slosh/build/slosh" "$PREFIX/bin/slosh"

say "done. run: slosh"
