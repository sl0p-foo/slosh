#!/usr/bin/env python3
"""A pane that rang, and the several places that have to say so.

A bell exists for the pane you are *not* looking at, so most of what matters is
where it shows up when the pane is off-screen, and that looking at it is what
puts it out.
"""
import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']
MARK = "\u2022"


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def lay(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


TWO_PANES = 'layout {\n tab name="api" {\n  pane\n  pane\n }\n}\n'
TWO_TABS = ('layout {\n tab name="api" {\n  pane\n  pane\n }\n'
            ' tab name="notes" {\n  pane\n }\n}\n')


def ring(s, pane_id, back_to):
    """Ring the bell in a pane while looking at another one."""
    s.api("focus", id=pane_id)
    s.settle(10)
    s.raw(r"\x07")
    s.settle(30)
    s.api("focus", id=back_to)
    s.settle(30)


def titlebar(snap, p, n=10):
    return snap.line(p["y"])[p["x"]:p["x"] + n]


def test_a_bel_marks_the_pane_that_rang():
    l = lay(TWO_PANES)
    with Session(SH, cols=90, rows=16, layout=l) as s:
        s.settle(30)
        panes = s.panes()
        mine = [p for p in panes if p["focused"]][0]
        other = [p for p in panes if not p["focused"]][0]

        check("no mark before anything rings",
              MARK not in titlebar(s.snapshot(), other), "")

        ring(s, other["id"], mine["id"])
        check("the pane that rang is marked",
              MARK in titlebar(s.snapshot(), other),
              repr(titlebar(s.snapshot(), other)))
        check("and the pane that did not is untouched",
              MARK not in titlebar(s.snapshot(), mine),
              repr(titlebar(s.snapshot(), mine)))
    os.unlink(l)


def test_looking_at_it_is_what_puts_it_out():
    l = lay(TWO_PANES)
    with Session(SH, cols=90, rows=16, layout=l) as s:
        s.settle(30)
        panes = s.panes()
        mine = [p for p in panes if p["focused"]][0]
        other = [p for p in panes if not p["focused"]][0]

        ring(s, other["id"], mine["id"])
        check("rung and unlooked-at", MARK in titlebar(s.snapshot(), other), "")

        s.api("focus", id=other["id"])
        s.settle(30)
        check("focusing it clears the mark",
              MARK not in titlebar(s.snapshot(), other),
              repr(titlebar(s.snapshot(), other)))

        # It must stay out; a bell is not a thing that comes back on its own.
        s.api("focus", id=mine["id"])
        s.settle(30)
        check("and it stays cleared once answered",
              MARK not in titlebar(s.snapshot(), other),
              repr(titlebar(s.snapshot(), other)))
    os.unlink(l)


def test_a_bell_in_another_tab_shows_on_the_strip():
    """The case the indicator exists for: the pane is not on screen at all."""
    l = lay(TWO_TABS)
    with Session(SH, cols=90, rows=16, layout=l) as s:
        s.settle(30)
        tabs = s.tabs()
        here = [p for p in s.panes() if p["focused"]][0]
        away = [p for p in s.panes() if p["tab"] == 2][0]

        check("the strip is unmarked to start",
              MARK not in s.snapshot().line(1), repr(s.snapshot().line(1)))

        s.api("select-tab", id=tabs[1]["id"])
        s.settle(20)
        s.api("focus", id=away["id"])
        s.settle(10)
        s.raw(r"\x07")
        s.settle(30)
        s.api("select-tab", id=tabs[0]["id"])
        s.api("focus", id=here["id"])
        s.settle(30)

        strip = s.snapshot().line(1)
        check("the tab holding the rung pane is marked", MARK in strip,
              repr(strip))
        check("and the mark sits with that tab's own label",
              strip.index(MARK) > strip.index("notes"), repr(strip))

        s.api("select-tab", id=tabs[1]["id"])
        s.settle(30)
        check("visiting the tab clears it",
              MARK not in s.snapshot().line(1), repr(s.snapshot().line(1)))
    os.unlink(l)


def test_the_strip_and_the_pane_clear_together():
    """The acknowledgement happens before the frame is drawn, so the strip --
    which is painted first -- cannot spend a frame reporting a stale bell."""
    l = lay(TWO_PANES)
    with Session(SH, cols=90, rows=16, layout=l) as s:
        s.settle(30)
        panes = s.panes()
        mine = [p for p in panes if p["focused"]][0]
        other = [p for p in panes if not p["focused"]][0]
        ring(s, other["id"], mine["id"])

        s.api("focus", id=other["id"])
        s.settle(30)
        snap = s.snapshot()
        check("neither the titlebar nor the strip is left marked",
              MARK not in titlebar(snap, other) and MARK not in snap.line(1),
              repr(titlebar(snap, other)) + " / " + repr(snap.line(1)))
    os.unlink(l)


def test_it_can_be_turned_off_and_the_mark_chosen():
    l = lay(TWO_PANES)
    off = cfg("bell_indicator false\n")
    with Session(SH, cols=90, rows=16, config=off, layout=l) as s:
        s.settle(30)
        panes = s.panes()
        mine = [p for p in panes if p["focused"]][0]
        other = [p for p in panes if not p["focused"]][0]
        ring(s, other["id"], mine["id"])
        snap = s.snapshot()
        check("bell_indicator false says nothing anywhere",
              MARK not in titlebar(snap, other) and MARK not in snap.line(1),
              repr(titlebar(snap, other)))
    os.unlink(off)

    custom = cfg('bell_mark "!"\n')
    with Session(SH, cols=90, rows=16, config=custom, layout=l) as s:
        s.settle(30)
        panes = s.panes()
        mine = [p for p in panes if p["focused"]][0]
        other = [p for p in panes if not p["focused"]][0]
        ring(s, other["id"], mine["id"])
        check("a chosen mark is the one drawn",
              "!" in titlebar(s.snapshot(), other),
              repr(titlebar(s.snapshot(), other)))
    os.unlink(custom)
    os.unlink(l)


# ---- a pane you have put away ----------------------------------------------

RINGER = ["/bin/sh", "-c",
          'printf "\\033]2;ringer\\007"; sleep 1; printf "\\007"; '
          'stty raw -echo; cat']


def bar_row(s):
    open_panes = [p for p in s.panes() if p["w"] > 1]
    return max(p["y"] + p["h"] for p in open_panes)


def test_a_minimised_pane_rings_on_the_bar():
    """The pane is not on screen at all, which is the case a bell is for.

    It has to ring itself on a delay: `raw` writes to the focused pane, and
    focusing a minimised one is what restores it.
    """
    with Session(RINGER, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        gone = s.panes()[0]["id"]
        s.api("focus", id=gone)
        s.settle(10)
        s.send(r"\x01m")           # put it away before it rings
        s.settle(20)
        check("it is on the bar and not on screen",
              [p["w"] for p in s.panes() if p["id"] == gone] == [0],
              str(s.panes()))
        row = s.snapshot().text[bar_row(s)]
        check("with no mark yet", MARK not in row, repr(row))

        for _ in range(60):        # gate on the mark rather than guess at it
            s.settle(30)
            if MARK in s.snapshot().text[bar_row(s)]:
                break
        row = s.snapshot().text[bar_row(s)]
        check("ringing while put away marks its entry", MARK in row, repr(row))
        check("and the tab it lives in", MARK in s.snapshot().line(1),
              repr(s.snapshot().line(1)))

        entry = [h for h in s.snapshot().hits
                 if h["action"] == f"focus:{gone}" and h["h"] == 1]
        check("the entry is still one target", len(entry) == 1, str(entry))
        if not entry:
            return
        s.click(entry[0]["x"] + 1, entry[0]["y"])
        s.settle(30)
        check("restoring it answers the bell everywhere",
              MARK not in s.snapshot().line(1)
              and [p["w"] for p in s.panes() if p["id"] == gone] != [0],
              repr(s.snapshot().line(1)))


if __name__ == "__main__":
    test_a_bel_marks_the_pane_that_rang()
    test_looking_at_it_is_what_puts_it_out()
    test_a_bell_in_another_tab_shows_on_the_strip()
    test_the_strip_and_the_pane_clear_together()
    test_it_can_be_turned_off_and_the_mark_chosen()
    test_a_minimised_pane_rings_on_the_bar()
    sys.exit(report())
