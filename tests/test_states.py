#!/usr/bin/env python3
"""What slosh says, out of the box, about a pane you are not in.

Three states mean "these cells are not a running program's present": its
program exited, it never started, or you are looking at the past. None of them
is discoverable by looking unless something says so, which is the whole
argument for shading them, so all three are shaded by default.

Two states mean "this is not the pane you are in" — unfocused, and one being
dragged. Those are ambient contrast, which is a taste, and a taste shipped as
a default is how a tool gets a reputation for fighting you. This file asserts
that line in both directions: the first three are coloured, the fourth is not.
"""

import os
import sys
import tempfile

from harness import Session, check, report

# A pane with a screenful of known-coloured text.
SH = [
    "/bin/sh",
    "-c",
    'printf "\\033]2;p\\007"; i=0; '
    'while [ $i -lt 40 ]; do printf "line %s ABCDEFGHIJKLMNOP\\n" $i; i=$((i+1)); done; '
    "read x",
]

# Only the theme, so "what colour is this cell" has one answer. No states: the
# point is what slosh does when you have said nothing.
CFG = None


def cfg():
    global CFG
    if CFG is None:
        f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
        f.write('theme { default_fg "#ffffff" default_bg "#000000" }\n')
        f.close()
        CFG = f.name
    return CFG


def fg(snap, pane, row=0, col=0):
    run = snap.style_at(pane["content_x"] + col, pane["content_y"] + row)
    return (run or {}).get("fg")


def test_a_dead_pane_is_coloured_by_default():
    # A *commanded* pane: only those keep their corpse by default, which is
    # the only kind there is to colour.
    lay = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    lay.write(
        "layout {\n  tab {\n"
        "    pane command=\"printf '\\\\033]2;p\\\\007'; echo DONE; exit 3\"\n"
        "  }\n}\n"
    )
    lay.close()
    with Session(SH, cols=50, rows=10, config=cfg(), layout=lay.name) as s:
        snap = s.until_text("[process exited")
        check(
            "its contents are not left looking live",
            fg(snap, s.pane()) not in (None, "#ffffff"),
            str(fg(snap, s.pane())),
        )
    os.unlink(lay.name)


def test_a_suspended_pane_is_coloured_by_default():
    layout = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    layout.write(
        'layout {\n  tab name="t" {\n'
        '    pane command="echo hello; read x" suspended=true\n'
        "  }\n}\n"
    )
    layout.close()
    with Session(SH, cols=60, rows=12, config=cfg(), layout=layout.name) as s:
        s.settle(60)
        pane = s.pane()
        snap = s.snapshot()
        check(
            "a pane that never started does not look like one that did",
            fg(snap, pane) not in (None, "#ffffff"),
            f"{fg(snap, pane)} — {snap.pane_text(pane)[:40]!r}",
        )
    os.unlink(layout.name)


def test_scrollback_is_coloured_by_default():
    with Session(SH, cols=60, rows=12, config=cfg()) as s:
        s.until_text("line 39")
        pane = s.pane()
        live = fg(s.snapshot(), pane)

        s.send(r"\x01\x1b[5~")  # C-a PgUp: into the scrollback
        s.settle(20)
        past = fg(s.snapshot(), pane)
        check(
            "the pane is scrolled",
            s.panes() and "▲" in s.snapshot().screen(),
            s.snapshot().screen()[:200],
        )
        check(
            "looking at the past looks different from the present",
            past != live,
            f"live {live}, scrolled {past}",
        )

        s.send(r"\x01\x1b[F")  # C-a End: back to the bottom
        s.settle(20)
        check(
            "and it goes back when you do",
            fg(s.snapshot(), pane) == live,
            f"{fg(s.snapshot(), pane)} vs {live}",
        )


def test_an_unfocused_pane_is_dimmed_by_default():
    """This used to assert the opposite, deliberately: ambient contrast is a
    taste, and the argument against shipping a taste was that turning it off
    should be obvious. It is now — `dim_unfocused 0` — which is what changed.
    The knob is the escape hatch that makes the default defensible."""
    with Session(SH, cols=90, rows=14, config=cfg()) as s:
        s.until_text("line 39")
        before = fg(s.snapshot(), s.pane())
        s.key("\\\\")  # split; the left pane loses focus
        s.settle(40)
        unfocused = [p for p in s.panes() if not p["focused"]][0]
        after = fg(s.snapshot(), unfocused)
        check(
            "the pane you left is pushed back", after != before, f"{before} -> {after}"
        )
        check("but still plainly readable", after not in (None, "#000000"), str(after))


def test_dim_unfocused_zero_turns_it_off():
    conf = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    conf.write('theme { default_fg "#ffffff" default_bg "#000000" }\ndim_unfocused 0\n')
    conf.close()
    with Session(SH, cols=90, rows=14, config=conf.name) as s:
        s.until_text("line 39")
        before = fg(s.snapshot(), s.pane())
        s.key("\\\\")
        s.settle(40)
        unfocused = [p for p in s.panes() if not p["focused"]][0]
        check(
            "nothing is done to it",
            fg(s.snapshot(), unfocused) == before,
            f"{before} -> {fg(s.snapshot(), unfocused)}",
        )
    os.unlink(conf.name)


def test_a_states_block_beats_the_knob():
    """Both directions, asserted on the colour rather than on "did it change":
    a chain replaces the knob's dim, and an empty block means "nothing" rather
    than "you did not say". (A grayscale chain makes a poor probe here — it
    leaves white text white, so it looks like nothing happened.)"""
    cases = [
        (
            'dim_unfocused 60\nstates { unfocused { tint amount=255 color="#00ff00" } }\n',
            "#00ff00",
            "a chain replaces the knob",
        ),
        (
            "dim_unfocused 60\nstates { unfocused { } }\n",
            None,
            "an empty block means nothing at all",
        ),
        ("dim_unfocused 60\n", "#c3c3c3", "and the knob applies when nobody said"),
    ]
    for text, want, label in cases:
        conf = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
        conf.write('theme { default_fg "#ffffff" default_bg "#000000" }\n' + text)
        conf.close()
        with Session(SH, cols=90, rows=14, config=conf.name) as s:
            s.until_text("line 39")
            s.key("\\\\")
            s.settle(40)
            unfocused = [p for p in s.panes() if not p["focused"]][0]
            got = fg(s.snapshot(), unfocused)
            check(label, got == want, f"{got}, wanted {want}")
        os.unlink(conf.name)


def test_a_flattened_tab_still_says_which_panes_are_not_live():
    """A collapsed header is chrome, and the shader pass never touches chrome
    (D13) — so the words have to carry what the colour carries everywhere
    else. Collapsing a tab must not hide the fact."""
    layout = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    layout.write(
        'layout {\n  tab name="t" {\n'
        '    pane command="stty raw -echo; cat"\n'
        '    pane command="echo hi; read x" suspended=true\n'
        "  }\n}\n"
    )
    layout.close()
    with Session(SH, cols=100, rows=14, config=cfg(), layout=layout.name) as s:
        s.settle(60)
        s.resize(40, 12)  # too small for two: the tab flattens
        s.settle(40)
        screen = s.snapshot().screen()
        check("the header says the pane never started", "not started" in screen, screen)
    os.unlink(layout.name)


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
