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

`contrib/vendor-check` says when that is worth doing: it compares the pin in
`vendor/*.vendor.json` against upstream and lists the commits touching the
terminal core, so watching upstream is one command rather than a browser tab.
`ship` mentions it after every release.

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

## 0002 — don't export weak `calloc`/`free` from the static library

`pkg/wuffs/src/main.zig` exported weak, hidden `calloc`/`free` stubs whenever
`!builtin.link_libc`. `callocStub` returns `null` unconditionally. They exist
because wuffs' generated `wuffs_foo__bar__alloc()` helpers are the only code
referencing libc's allocator, we never call them, and linker GC *usually*
strips them — the stubs are there in case it doesn't.

The comment says "weak so that any real definition wins". That holds when the
real definition comes from a shared libc, but not from a static archive: once
the linker pulls this member in for some other symbol, the weak `calloc` here
*satisfies* the reference, so it never searches `libc.a` for musl's strong
one. Every `calloc` in the consuming program then returns `NULL`, and `free`
becomes a no-op.

We only hit this in the browser demo, which is the one build that has to pass
`-Dsimd=false` (highway emits RVV intrinsics that do not compile for rv64gc),
and `GhosttyZig.zig` sets `link_libc = if (cfg.simd) true else null` — so the
demo's library is the only one of ours built without libc, and the guest links
it statically against musl. The symptom was `slosh: cannot start session main`
in the demo: the first `calloc` in `kdl.c:node_new` returned `NULL` and the
store into it faulted, which killed the session server before its socket
existed. The host build (libc linked, stubs never exported) was unaffected,
which is why every test on this machine passed.

The patch adds `and builtin.output_mode == .Exe`. The stubs still do their job
in a final link with no libc, and a library — whose consumer is the thing that
owns the choice of allocator — no longer interposes on it. `web/demo/boot-check`
is the regression test; it caught this in twenty seconds once it was run.

Worth sending upstream.
