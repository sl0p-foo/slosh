#!/usr/bin/env python3
"""A pane's program is told how big the pane is.

A pane is born 1x1 and given its real size by the layout a moment later, so
everything a program knows about its own width and height arrives through that
second message. If it is dropped, the program spends its whole life believing
it has one row and one column -- which is a shell wrapping every prompt and an
editor drawing its screen into a corner.

Darwin drops it if you are not careful: a pty master has no line discipline
until its slave has been opened, so TIOCSWINSZ on the master returns ENOTTY,
and the fresh pane's resize races the child's open of the slave. Losing that
race is silent and permanent, so the size is asserted here rather than left to
be noticed in an editor.

`stty size` prints "rows cols", and the sleep is the point: it reads the size
*after* the layout has had every chance to deliver it.
"""

import sys

from harness import Session, check, report

# Report the size, then stay up so the pane is still there to be measured.
REPORT = ["/bin/sh", "-c", "sleep 0.3; stty size; read x"]


def reported(snap, pane):
    """The "rows cols" the program printed, as (rows, cols)."""
    for row in range(pane["content_h"]):
        line = snap.pane_line(pane, row).strip()
        parts = line.split()
        if len(parts) == 2 and all(p.isdigit() for p in parts):
            return int(parts[0]), int(parts[1])
    return None


def test_the_first_pane_is_told_its_size():
    with Session(REPORT, cols=80, rows=24) as s:
        snap = s.until(lambda snap: reported(snap, s.pane(0)) is not None)
        pane = s.pane(0)
        got = reported(snap, pane)
        check("the program was told a size at all", got is not None, snap.screen())
        check(
            "and it is the pane's size",
            got == (pane["content_h"], pane["content_w"]),
            f"program says {got}, pane is "
            f"{(pane['content_h'], pane['content_w'])}\n{snap.screen()}",
        )


def test_a_pane_from_a_split_is_told_its_size():
    """The split is where it hurts: the new pane's size exists only after the
    layout, so it is delivered to a child that is still starting up."""
    with Session(REPORT, cols=100, rows=30) as s:
        s.settle()
        s.key("\\\\")  # C-a \ -> split into columns
        s.until(lambda snap: len(s.panes()) == 2)
        snap = s.until(lambda snap: reported(snap, s.focused()) is not None)
        fresh = s.focused()
        got = reported(snap, fresh)
        check(
            "the fresh pane's program was told a size", got is not None, snap.screen()
        )
        check(
            "and it is the fresh pane's size",
            got == (fresh["content_h"], fresh["content_w"]),
            f"program says {got}, pane is "
            f"{(fresh['content_h'], fresh['content_w'])}\n{snap.screen()}",
        )


def test_a_resize_reaches_the_program():
    """The same message, after the pane has been running a while: this is what
    a window resize, a split beside it or a zoom all come down to."""
    with Session(["/bin/sh", "-c", "read x; stty size; read y"], cols=80, rows=24) as s:
        s.settle()
        s.resize(120, 40)
        s.settle()
        s.raw("\\n")
        snap = s.until(lambda snap: reported(snap, s.pane(0)) is not None)
        pane = s.pane(0)
        got = reported(snap, pane)
        check("the program saw the new size", got is not None, snap.screen())
        check(
            "and it matches the pane",
            got == (pane["content_h"], pane["content_w"]),
            f"program says {got}, pane is "
            f"{(pane['content_h'], pane['content_w'])}\n{snap.screen()}",
        )


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
