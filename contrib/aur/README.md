# The AUR package

This directory is the upstream copy of the [AUR](https://aur.archlinux.org)
package: the PKGBUILD, and the `.SRCINFO` generated from it. The AUR itself
is a separate git repo — `ssh://aur@aur.archlinux.org/slosh.git` — that
receives these two files; nothing else goes there.

The package builds from the cgit snapshot of a release tag
(`https://git.sl0p.foo/slosh.git/snapshot/slosh-vX.Y.Z.tar.gz`), which
carries the vendored libghostty-vt. The build fetches nothing: every zig
dependency in the vendored core's `build.zig.zon` is lazy and unused by the
VT-only build, so it is clean under makechrootpkg's offline build. zig 0.16
comes from `extra/zig`.

## Updating on a release

After `contrib/release X.Y.Z` (which prints this as a reminder):

```sh
cd contrib/aur
curl -fsSLO https://git.sl0p.foo/slosh.git/snapshot/slosh-vX.Y.Z.tar.gz
sha256sum slosh-vX.Y.Z.tar.gz     # -> pkgver and sha256sums in PKGBUILD
rm slosh-vX.Y.Z.tar.gz
makepkg --printsrcinfo > .SRCINFO
makepkg -f && rm -rf src pkg *.pkg.tar.zst   # build it once, actually
```

Commit here, then mirror to the AUR repo:

```sh
git clone ssh://aur@aur.archlinux.org/slosh.git /tmp/aur-slosh
cp PKGBUILD .SRCINFO /tmp/aur-slosh/
cd /tmp/aur-slosh && git add -A && git commit -m "X.Y.Z" && git push
```

Pushing needs an AUR account with an SSH key
([wiki](https://wiki.archlinux.org/title/AUR_submission_guidelines)); the
first push creates the package.

## Testing without the system zig

The PKGBUILD resolves zig from `/usr/bin/zig` (what `makedepends` installs),
but honours `$ZIG` so it can be built where zig lives elsewhere:

```sh
ZIG=$HOME/zig-0.16.0/zig makepkg -f -d    # -d: zig is not pacman-installed
```
