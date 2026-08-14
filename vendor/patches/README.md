# Patches to the vendored libghostty-vt

D12 vendors libghostty-vt pinned by commit, "with a patches file if we ever
need one". This is that file.

Patches are applied **to the vendored tree in this repository**, which is
committed, so a normal `make vendor` build already has them. The `.patch`
files here exist for provenance: they say what we changed, why, and what has
to be re-applied when the vendored version is bumped. `vendor/*.vendor.json`
lists them under `patches`.

Re-vendoring is therefore: drop in the new upstream tree, apply each patch in
order, resolve anything that has moved, rebuild, run `make test`.

## 0001 — keep kitty images across a screen clear

`Terminal.eraseDisplay` called `kitty_images.delete(..., .{ .all = true })`
for both `ED 2` and `ED 3`, where the `true` means "and free the image data
for anything no longer placed".

The kitty graphics protocol separates *transmitting* an image (`a=t`, which
stores it under an id) from *placing* it (`a=p`, which puts it on screen), so
that a program can upload once and place many times — that is the whole point
of having two verbs, and the reason the delete command distinguishes `d=a`
(placements) from `d=A` (placements and data). Freeing the data on a screen
clear breaks it: any full-screen program that clears and redraws loses every
image it uploaded, and every later placement of that id draws nothing at all.

Found with a bouncing-DVD screensaver that transmits ten frames up front and
then places one per frame. It works in kitty; under us it drew nothing, and
the reason was three layers down.

The patch removes only the freeing. Placements on the cleared screen still go
— clearing the screen should clear what is on it — and the image store still
bounds itself the way it always did, with a 320MB limit and LRU eviction on
insert, which is exactly the mechanism that makes keeping them safe.

Worth sending upstream.
