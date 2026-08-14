#!/usr/bin/env python3
"""Putting a pane away.

A minimised pane leaves the layout and sits in a strip along the bottom, one
row each, still running. It is intent rather than a derived state, so the
things worth checking are the same ones zoom needed: that it survives the
layout being recomputed, that there is always a way back, and that it does not
invent a state with nothing on screen.
"""
import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def here(s):
    """Panes of the current tab; another tab's are not laid out."""
    active = [t["index"] for t in s.tabs() if t["active"]][0]
    return [p for p in s.panes() if p["tab"] == active]


def strip(s):
    return [p for p in here(s) if p["h"] == 1]


def open_panes(s):
    return [p for p in here(s) if p["h"] > 1]


def test_minimising_moves_a_pane_to_the_strip():
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        before = sorted(p["w"] for p in open_panes(s))

        s.send(r"\x01m")
        s.settle(20)
        check("one pane is now a single row", len(strip(s)) == 1,
              str(here(s)))
        check("and the other still has the tab", len(open_panes(s)) == 1,
              str(here(s)))
        check("which is wider than it was, having taken the space back",
              open_panes(s)[0]["w"] > max(before), str(here(s)))

        row = strip(s)[0]
        rest = open_panes(s)[0]
        check("the strip sits under the layout, not in it",
              row["y"] > rest["y"] + rest["h"] - 1, f"{row} vs {rest}")
        check("and spans the width", row["w"] == rest["w"], str(here(s)))


def test_clicking_the_row_brings_it_back():
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        layout_before = sorted((p["x"], p["w"]) for p in open_panes(s))

        s.send(r"\x01m")
        s.settle(20)
        row = strip(s)[0]
        s.click(row["x"] + 4, row["y"])
        s.settle(20)
        check("clicking the row restores the pane", strip(s) == [], str(here(s)))
        check("and the layout is the one it left",
              sorted((p["x"], p["w"]) for p in open_panes(s)) == layout_before,
              str(here(s)))


def test_focusing_it_by_any_route_brings_it_back():
    """Restoring is not a special verb: a focused pane is never minimised, and
    that is checked once a frame rather than at each place focus can move."""
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.send(r"\x01m")
        s.settle(20)
        gone = strip(s)[0]["id"]

        s.api("focus", id=gone)
        s.settle(20)
        check("focusing it over the control API restores it",
              strip(s) == [], str(here(s)))


def test_it_will_not_leave_the_tab_empty():
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        check("one pane to start", len(here(s)) == 1, str(here(s)))
        s.send(r"\x01m")
        s.settle(20)
        check("minimising the only pane is refused", strip(s) == [],
              str(here(s)))
        check("and it says why", "nothing else" in s.snapshot().screen(),
              repr(s.snapshot().screen()[-200:]))

        s.api("split", dir="cols")
        s.settle(20)
        s.send(r"\x01m")
        s.settle(20)
        check("with two, one can go", len(strip(s)) == 1, str(here(s)))
        s.send(r"\x01m")
        s.settle(20)
        check("but the last one standing cannot", len(strip(s)) == 1,
              str(here(s)))


def test_the_button_minimises_that_pane():
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        target = here(s)[0]["id"]
        b = [h for h in s.snapshot().hits
             if h["action"] == f"minimize:{target}"]
        check("a pane's frame carries a minimise button", len(b) == 1, str(b))
        if not b:
            return
        s.click(b[0]["x"] + 1, b[0]["y"])
        s.settle(20)
        check("clicking it puts that pane away",
              [p["id"] for p in strip(s)] == [target], str(here(s)))


def test_it_has_no_say_once_the_tab_is_a_list():
    """Down there every pane is a row already, so being minimised is not a
    different thing to be -- which is the whole reason it costs nothing."""
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.send(r"\x01m")
        s.settle(20)
        check("one minimised, one open", len(strip(s)) == 1, str(here(s)))

        s.resize(26, 20)          # narrow enough that the tab flattens
        s.settle(20)
        rows = here(s)
        check("the tab is a list", any(p["hidden"] for p in rows), str(rows))
        check("and the minimised pane is just another row in it",
              len([p for p in rows if p["h"] == 1]) == len(rows) - 1,
              str(rows))

        s.resize(90, 20)
        s.settle(20)
        check("coming back, it is minimised again and not lost",
              len(strip(s)) == 1 and len(open_panes(s)) == 1, str(here(s)))


def test_zoom_hides_the_strip():
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.api("split", dir="rows")
        s.settle(20)
        s.send(r"\x01m")
        s.settle(20)
        check("a strip to start", len(strip(s)) == 1, str(here(s)))
        s.send(r"\x01z")
        s.settle(20)
        check("zooming shows one pane and nothing else, strip included",
              len([p for p in here(s) if p["w"] > 1]) == 1, str(here(s)))
        s.send(r"\x01z")
        s.settle(20)
        check("and unzooming gives the strip back", len(strip(s)) == 1,
              str(here(s)))


if __name__ == "__main__":
    test_minimising_moves_a_pane_to_the_strip()
    test_clicking_the_row_brings_it_back()
    test_focusing_it_by_any_route_brings_it_back()
    test_it_will_not_leave_the_tab_empty()
    test_the_button_minimises_that_pane()
    test_it_has_no_say_once_the_tab_is_a_list()
    test_zoom_hides_the_strip()
    sys.exit(report())
