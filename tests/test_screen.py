#!/usr/bin/env python3
"""Composition, styling, cursor, scrollback, resize, and the input round trip.

Everything here runs through the headless driver, so it is deterministic and
takes milliseconds. Contrast tests/live_*.py, which exist only for the parts
that genuinely need a tty.
"""
import sys

from harness import Session, check, report

CAT = ["/bin/sh", "-c", "stty raw -echo; cat -v"]
SH = lambda script: ["/bin/sh", "-c", script]


def test_text_and_cursor():
    with Session(SH("stty raw -echo; cat"), cols=20, rows=3) as s:
        s.settle()
        s.raw("hello")
        s.settle()
        snap = s.snapshot()
        check("text lands on row 0", snap.line(0).startswith("hello"), repr(snap.line(0)))
        check("cursor follows the text", snap.cursor == {"visible": True, "x": 5, "y": 0},
              str(snap.cursor))

        s.raw("\\r\\nsecond")
        s.settle()
        snap = s.snapshot()
        check("newline moves to row 1", snap.line(1).startswith("second"), repr(snap.line(1)))
        check("cursor on row 1", (snap.cursor["x"], snap.cursor["y"]) == (6, 1),
              str(snap.cursor))


def test_styles():
    with Session(SH(r'printf "n\033[1mB\033[31mR\033[0mn"; sleep 5'), cols=10, rows=2) as s:
        s.settle()
        snap = s.snapshot()
        check("style: plain cell has no run", snap.style_at(0, 0) is None,
              str(snap.style_at(0, 0)))
        b = snap.style_at(1, 0)
        check("style: bold is recorded", b and b["attrs"] == ["bold"], str(b))
        r = snap.style_at(2, 0)
        check("style: colour is resolved to rgb",
              r and r["fg"] is not None and "bold" in r["attrs"], str(r))
        check("style: reset ends the run", snap.style_at(3, 0) is None,
              str(snap.style_at(3, 0)))


def test_wide_and_clusters():
    with Session(SH('printf "[\\346\\227\\245]"; sleep 5'), cols=10, rows=1) as s:
        s.settle()
        snap = s.snapshot()
        check("wide char occupies one cell in the dump", snap.line(0).startswith("[日]"),
              repr(snap.line(0)))


def test_scrollback():
    with Session(SH("stty raw -echo; cat"), cols=20, rows=3) as s:
        s.settle()
        s.raw("1\\r\\n2\\r\\n3\\r\\n4\\r\\n5")
        s.settle()
        snap = s.snapshot()
        check("viewport shows the tail, not the head",
              snap.line(0).startswith("3") and snap.line(2).startswith("5"),
              repr(snap.text))


def test_resize():
    with Session(SH("stty raw -echo; cat"), cols=20, rows=3) as s:
        s.settle()
        s.resize(40, 5)
        s.settle()
        snap = s.snapshot()
        check("resize changes the composited size",
              snap.cols == 40 and snap.rows == 5 and len(snap.line(0)) == 40,
              f"{snap.cols}x{snap.rows} line={len(snap.line(0))}")

        s.raw("\\033[999;999H\\033[6n")  # ask the pane where it thinks it is
        s.settle()
        check("pane still responds after resize", s.alive())


def test_input_round_trip():
    """The decoder and the per-pane encoder, without a pty in sight."""
    with Session(CAT, cols=40, rows=4) as s:
        s.settle()
        s.send(r"\e[1;5A")  # legacy ctrl-up
        s.settle()
        check("legacy key reaches the pane", "^[[1;5A" in s.snapshot().screen())

    with Session(CAT, cols=40, rows=4) as s:
        s.settle()
        s.send(r"\e[98;5u")  # kitty ctrl-b -> legacy pane
        s.settle()
        check("kitty key is re-encoded for a legacy pane",
              "^B" in s.snapshot().screen())

    with Session(CAT, cols=40, rows=4) as s:
        s.settle()
        s.send(r"\e[200~pasted\e[201~")
        s.settle()
        check("paste is unwrapped for a pane that did not ask",
              "pasted" in s.snapshot().screen())

    kitty = ["/bin/sh", "-c",
             'stty raw -echo; printf "\\033[>1u\\033[?1000h\\033[?1006h"; cat -v']
    with Session(kitty, cols=40, rows=4) as s:
        s.settle()
        s.send(r"\x02")  # legacy ctrl-b -> kitty pane
        s.settle()
        check("legacy key is re-encoded for a kitty pane",
              "^[[98;5u" in s.snapshot().screen(), s.snapshot().screen())

        s.send(r"\e[<0;7;3M")
        s.settle()
        check("mouse reaches a pane that asked for it",
              "^[[<0;7;3M" in s.snapshot().screen())


def test_hitlist_contract():
    """Empty until M1 paints something clickable, but the contract is live."""
    with Session(SH("sleep 5"), cols=10, rows=2) as s:
        s.settle()
        snap = s.snapshot()
        check("hit list exists and is empty", snap.hits == [], str(snap.hits))
        check("hit test on empty list is None", snap.hit_at(0, 0) is None)


if __name__ == "__main__":
    test_text_and_cursor()
    test_styles()
    test_wide_and_clusters()
    test_scrollback()
    test_resize()
    test_input_round_trip()
    test_hitlist_contract()
    sys.exit(report())
