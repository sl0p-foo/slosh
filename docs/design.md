# How it works

Two decisions do most of the work. The rest — twenty numbered decisions, each
with what it cost to get wrong — is in
`DESIGN.md`.

## One geometry

**Everything drawn registers what it is, as it is drawn**, and a click is a
lookup in that list. Drawing and hit-testing cannot disagree, because there is
only one of them. The dashed guide that shows where a split would land is drawn
from the same rect the click will resolve against, in the same pass — so a
preview that lies is not a bug you can have.

The same rule applies to state that could be derived: the layout is a pure
function of the tree and the terminal's size, recomputed every frame. Nothing
stores where a pane is. That is why a small terminal can collapse a tab to a list
of headers and come back to exactly the layout you had, and why a pane cannot be
left greyed out by the one code path that forgot to clear a flag.

Sizes are **weights**, so an even split is equal weights and resizing is not a
special case of anything.

## It runs headless

`sl0ppty --script` is the whole thing without a terminal: commands in, the
composited screen out as JSON. The interactive client and the test suite drive
the same object through the same commands, so there is exactly one implementation
of what a key does.

That is what makes the tests real: they type keys and assert on the screen a
client would have seen, rather than on the internals. 1,500-odd checks in about
thirteen seconds, plus 260 more from unit tests on the parts that are pure
functions (the KDL parser, the shader pass, the expression compiler).

## The shape of it

```
src/app.c      the session: tree, layout, chrome, input routing, control API
src/pane.c     one pty + one libghostty-vt terminal, composed into the screen
src/screen.c   our cell buffer, and the minimal byte stream to update a terminal
src/server.c   the session process: socket, poll loop, config watcher
src/client.c   the attached terminal: bytes in, bytes out
src/config.c   the config, the keymap, the cheatsheet's names
src/shader.c   the colour passes; src/expr.c compiles their strengths
src/kdl.c      a hand-rolled KDL subset, for configs and layouts
```

About 15k lines of C. The vendored terminal core does VT parsing, scrollback,
selection, images and key encoding; everything above is ours.
