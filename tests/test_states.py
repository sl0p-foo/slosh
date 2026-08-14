#!/usr/bin/env python3
"""What sl0ppty says, out of the box, about a pane that is not live.

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
SH = ["/bin/sh", "-c",
      'printf "\\033]2;p\\007"; i=0; '
      'while [ $i -lt 40 ]; do printf "line %s ABCDEFGHIJKLMNOP\\n" $i; i=$((i+1)); done; '
      'read x']

# Only the theme, so "what colour is this cell" has one answer. No states: the
# point is what sl0ppty does when you have said nothing.
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
    with Session(["/bin/sh", "-c", 'printf "\\033]2;p\\007"; echo DONE; exit 3'],
                 cols=50, rows=10, config=cfg()) as s:
        snap = s.until_text("[process exited")
        check("its contents are not left looking live",
              fg(snap, s.pane()) not in (None, "#ffffff"), str(fg(snap, s.pane())))


def test_a_suspended_pane_is_coloured_by_default():
    layout = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    layout.write('layout {\n  tab name="t" {\n'
                 '    pane command="echo hello; read x" suspended=true\n'
                 '  }\n}\n')
    layout.close()
    with Session(SH, cols=60, rows=12, config=cfg(), layout=layout.name) as s:
        s.settle(60)
        pane = s.pane()
        snap = s.snapshot()
        check("a pane that never started does not look like one that did",
              fg(snap, pane) not in (None, "#ffffff"),
              f"{fg(snap, pane)} — {snap.pane_text(pane)[:40]!r}")
    os.unlink(layout.name)


def test_scrollback_is_coloured_by_default():
    with Session(SH, cols=60, rows=12, config=cfg()) as s:
        s.until_text("line 39")
        pane = s.pane()
        live = fg(s.snapshot(), pane)

        s.send(r"\x01\x1b[5~")          # C-a PgUp: into the scrollback
        s.settle(20)
        past = fg(s.snapshot(), pane)
        check("the pane is scrolled", s.panes() and "▲" in s.snapshot().screen(),
              s.snapshot().screen()[:200])
        check("looking at the past looks different from the present",
              past != live, f"live {live}, scrolled {past}")

        s.send(r"\x01\x1b[F")           # C-a End: back to the bottom
        s.settle(20)
        check("and it goes back when you do", fg(s.snapshot(), pane) == live,
              f"{fg(s.snapshot(), pane)} vs {live}")


def test_an_unfocused_pane_is_left_alone():
    """The deliberate non-opinion. If this ever fails because someone shipped a
    default dim, that was a taste, and it needs an argument rather than a
    commit."""
    with Session(SH, cols=90, rows=14, config=cfg()) as s:
        s.until_text("line 39")
        before = fg(s.snapshot(), s.pane())
        s.key("\\\\")                   # split; the left pane loses focus
        s.settle(40)
        unfocused = [p for p in s.panes() if not p["focused"]][0]
        check("an unfocused pane keeps its colours",
              fg(s.snapshot(), unfocused) == before,
              f"{fg(s.snapshot(), unfocused)} vs {before}")


def test_a_flattened_tab_still_says_which_panes_are_not_live():
    """A collapsed header is chrome, and the shader pass never touches chrome
    (D13) — so the words have to carry what the colour carries everywhere
    else. Collapsing a tab must not hide the fact."""
    layout = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    layout.write('layout {\n  tab name="t" {\n'
                 '    pane command="stty raw -echo; cat"\n'
                 '    pane command="echo hi; read x" suspended=true\n'
                 '  }\n}\n')
    layout.close()
    with Session(SH, cols=100, rows=14, config=cfg(), layout=layout.name) as s:
        s.settle(60)
        s.resize(40, 12)                # too small for two: the tab flattens
        s.settle(40)
        screen = s.snapshot().screen()
        check("the header says the pane never started",
              "not started" in screen, screen)
    os.unlink(layout.name)


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
