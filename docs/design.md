# How it works

Two decisions do most of the work. The rest — twenty-two numbered decisions,
each with what it cost to get wrong — is in
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

`slosh --script` is the whole thing without a terminal: commands in, the
composited screen out as JSON. The interactive client and the test suite drive
the same object through the same commands, so there is exactly one implementation
of what a key does.

That is what makes the tests real: they type keys and assert on the screen a
client would have seen, rather than on the internals. 1,500-odd checks in about
thirteen seconds, plus 260 more from unit tests on the parts that are pure
functions (the KDL parser, the shader pass, the expression compiler).

## The shape of it

```
src/app.c      the session core: pane and tab lifecycle, input routing
src/app_layout.c    the layout: splits, tree edits, resizing, focus
src/app_draw.c      drawing the chrome, the shader passes, graphics
src/app_ui.c        the finder, the palette, modals, renaming
src/app_session.c   layout files, workspaces, dumping a session out
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

## How it is formatted

`clang-format` for the C (`.clang-format`) and `ruff` for the Python
(`ruff.toml`) -- imports sorted, then laid out. `make fmt` applies both,
`make fmt-check` only reports, and `make hooks` installs a pre-commit hook that
formats what you staged, so what lands in git is already formatted.

Getting the two tools:

```
make tools            # a venv in .venv with the pinned ruff
apt install clang-format          # ...or brew, or your package manager
```

clang-format is a system package everywhere. ruff usually is not, and
`pip install --user ruff` is refused outright on a distro python (PEP 668), so
`make tools` builds a venv in `.venv/` and everything looks there **before**
`$PATH`. That order matters: `ruff.toml` pins the version with ruff's own
`required-version`, so a system ruff of the wrong one refuses to run -- and
preferring `$PATH` would strand somebody whose `.venv` has exactly the version the
repo asked for. The venv survives `make clean`; `rm -rf .venv` starts over.

The pin is there because a formatter whose output moves between releases turns
"formatted" into "formatted by whoever committed last", and the diff lands on the
next person. A mismatch is refused with both versions named rather than quietly
reformatting the tree. Bumping it is one line in `ruff.toml`, one in the Makefile,
and a reformat commit -- which is the honest cost of a new version.

The base is LLVM, because that is what this code was written in by hand — two
spaces, 80 columns, `char *p`, braces attached. The settings that differ from it
were chosen by measuring the reformat rather than by taste: LLVM alone rewrote 38%
of the lines, mostly by splitting `if (!p) return false;` in two and reflowing
comments that are prose. Keeping those two and leaving include order alone brings
it to 17%, which is alignment and table packing.

Comments are not reflowed. They are prose here, wrapped by hand, and sometimes the
line breaks carry meaning — a paragraph, a list, a diagram of a pane. A comment
that runs long is a thing for a person to fix.

The Python is the opposite: left entirely at ruff's defaults. The C here was
hand-formatted into a house style worth arguing for; the Python was not, and a
formatter of that kind is only worth having when there is nothing left to argue
about. Every knob turned would be an argument to have again later. It cost a 60%
reformat of the test suite, once.

**Python imports are sorted and C includes are not**, which is not an
inconsistency. A C include order can be load-bearing -- `_GNU_SOURCE` ahead of the
system headers, and a grouping somebody wrote by hand -- while Python import order
is not. Where it genuinely is, a statement between the imports ends the block ruff
will sort, and the tests lean on exactly that: `sys.path.insert(...)` sits between
the standard library and `from harness import ...`, so the harness import cannot be
hoisted above the line that makes it importable.

The vendored terminal core is never formatted: it is pinned by commit (D12), and
reformatting it would turn every re-vendor into a merge.
