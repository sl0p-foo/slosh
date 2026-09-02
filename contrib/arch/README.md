# The Arch package

An AUR package was the plan, but AUR signups are closed while the AUR deals
with supply chain attacks — so this is self-hosted instead, which turns out
to be the better offer: a real pacman repository serves *built* packages,
where the AUR would have handed every user a rebuild.

Two ways in, both fed by the PKGBUILD in this directory:

**The repository** — binary packages, updated with `pacman -Syu` like
anything else. Packages and the database are signed, so first enroll the
signing key:

```sh
curl -fsSLO https://slosh.foo/arch/slosh.asc
sudo pacman-key --add slosh.asc
sudo pacman-key --lsign-key 0F67C909938CFFA8A76B59FD8D69B55A0E9056B9
```

That fingerprint is the thing to check before the `--lsign-key`: the same
key is `contrib/arch/slosh.asc` in git, so the file fetched from the
website can be compared against the one in the source — two channels that
would both have to lie. Then, in `/etc/pacman.conf`:

```ini
[slosh]
Server = https://slosh.foo/arch/$arch
```

and `pacman -Sy slosh`. No `SigLevel` override: pacman's default demands
package signatures, and now gets them. The database is signed too, so
`SigLevel = Required DatabaseRequired` also works, if that is your
temperament. (x86_64 today; aarch64 when a build machine of that shape
exists.)

**The PKGBUILD itself** — GitHub serves it raw, for anyone who would rather
build than trust:

```sh
curl -fsSLO https://raw.githubusercontent.com/sl0p-foo/slosh/master/contrib/arch/PKGBUILD
makepkg -si
```

The build is offline-clean: the source tarball is GitHub's archive of the
release tag, which carries the vendored libghostty-vt, and every zig
dependency in the vendored core's `build.zig.zon` is lazy and unused by the
VT-only build — `zig build` fetches nothing (verified against a cold
`ZIG_GLOBAL_CACHE_DIR`). zig 0.16 comes from `extra/zig`.

## The signing key

ed25519, sign-only, made for exactly this — `slosh package signing
<slosh@slosh.foo>`, fingerprint
`0F67C909938CFFA8A76B59FD8D69B55A0E9056B9`, expires 2028-08-28. The private
half lives in the release machine's GnuPG keyring and nowhere else;
`contrib/arch-repo` refuses to build the repo without it, because an
unsigned update would make every enrolled pacman refuse the repo. Before
the expiry: `gpg --quick-set-expire <fpr> 2y`, re-export
`contrib/arch/slosh.asc`, commit, republish — enrolled users pick the
extension up with `pacman-key --add` of the refreshed file (worth a note in
release notes when it happens). If the key is ever replaced rather than
extended, ship the successor the same two ways and say so loudly.

## Updating on a release

After `contrib/release X.Y.Z` (which prints this as a reminder):

1. Bump `pkgver` and `sha256sums` in the PKGBUILD — the hash of
   `https://github.com/sl0p-foo/slosh/archive/refs/tags/vX.Y.Z.tar.gz`.
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
