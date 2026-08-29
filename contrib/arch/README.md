# The Arch package

An AUR package was the plan, but AUR signups are closed while the AUR deals
with supply chain attacks — so this is self-hosted instead, which turns out
to be the better offer: a real pacman repository serves *built* packages,
where the AUR would have handed every user a rebuild.

Two ways in, both fed by the PKGBUILD in this directory:

**The repository** — binary packages, updated with `pacman -Syu` like
anything else. In `/etc/pacman.conf`:

```ini
[slosh]
SigLevel = Optional TrustAll
Server = https://slosh.foo/arch/$arch
```

then `pacman -Sy slosh`. (`SigLevel` spelled out because pacman's default
demands signatures, and these packages are not signed — yet. x86_64 today;
aarch64 when a build machine of that shape exists.)

**The PKGBUILD itself** — cgit serves it raw, for anyone who would rather
build than trust:

```sh
curl -fsSLO https://git.sl0p.foo/slosh.git/plain/contrib/arch/PKGBUILD
makepkg -si
```

The build is offline-clean: the source tarball is the cgit snapshot of the
release tag, which carries the vendored libghostty-vt, and every zig
dependency in the vendored core's `build.zig.zon` is lazy and unused by the
VT-only build — `zig build` fetches nothing (verified against a cold
`ZIG_GLOBAL_CACHE_DIR`). zig 0.16 comes from `extra/zig`.

## Updating on a release

After `contrib/release X.Y.Z` (which prints this as a reminder):

1. Bump `pkgver` and `sha256sums` in the PKGBUILD — the hash of
   `https://git.sl0p.foo/slosh.git/snapshot/slosh-vX.Y.Z.tar.gz`.
2. `contrib/arch-repo` — builds the package, updates the indexes in
   `dist/arch/<carch>/`, and prints the rsync that publishes it.
3. Commit the PKGBUILD change.

## When the AUR reopens

The PKGBUILD is AUR-ready as it stands: clean-chroot/offline safe, nothing
self-hosted about it but the hosting. Generate the metadata and push —

```sh
cd contrib/arch && makepkg --printsrcinfo > .SRCINFO
git clone ssh://aur@aur.archlinux.org/slosh.git /tmp/aur-slosh
cp PKGBUILD .SRCINFO /tmp/aur-slosh/
cd /tmp/aur-slosh && git add -A && git commit -m "X.Y.Z" && git push
```

— and keep the repository: the AUR entry is discoverability, the repository
is the better install.

## Testing without the system zig

The PKGBUILD resolves zig from `/usr/bin/zig` (what `makedepends` installs),
but honours `$ZIG` so it can be built where zig lives elsewhere:

```sh
ZIG=$HOME/zig-0.16.0/zig makepkg -f -d    # -d: zig is not pacman-installed
```

(`contrib/arch-repo` passes the same through, and adds `-d` on its own when
zig is not pacman-installed.)
