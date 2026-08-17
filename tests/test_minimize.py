#!/usr/bin/env python3
"""Putting a pane away.

A minimised pane leaves the layout entirely and is listed on a single row along
the bottom, whatever the number of them. It is intent rather than derived
state, so what is worth checking is that it survives the layout being
recomputed, that there is always a way back, and that it does not invent a
state with nothing on screen.
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


def open_panes(s):
    return [p for p in here(s) if p["w"] > 1]


def put_away(s):
    """Minimised panes get no rect at all: the bar draws them, not the layout."""
    return [p for p in here(s) if p["w"] <= 1]


def bar_row(s):
    """The bar's y, which is the row below every open pane."""
    op = open_panes(s)
    return max(p["y"] + p["h"] for p in op) if op else None


def bar_entries(s):
    y = bar_row(s)
    if y is None:
        return []
    return [
        h for h in s.snapshot().hits if h["y"] == y and h["action"].startswith("focus:")
    ]


def minimize_focused(s):
    s.send(r"\x01m")
    s.settle(20)


def test_minimising_takes_a_pane_out_of_the_layout():
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        before = max(p["w"] for p in open_panes(s))

        minimize_focused(s)
        check("one pane is gone from the layout", len(put_away(s)) == 1, str(here(s)))
        check(
            "and the other took the space back",
            open_panes(s)[0]["w"] > before,
            str(here(s)),
        )
        check("it is listed on the bar", len(bar_entries(s)) == 1, str(bar_entries(s)))
        check(
            "which sits below the panes",
            bar_entries(s)[0]["y"] == bar_row(s),
            str(bar_entries(s)),
        )


def test_the_bar_is_one_row_however_many_are_put_away():
    """A row each would let putting things away cost more room than having them
    out, which is the opposite of the point."""
    with Session(SH, cols=100, rows=22) as s:
        s.settle(20)
        for _ in range(3):
            s.api("split", dir="cols")
            s.settle(10)
        check("four panes", len(open_panes(s)) == 4, str(here(s)))

        minimize_focused(s)
        one = open_panes(s)[0]["h"]
        entries_one = len(bar_entries(s))
        minimize_focused(s)
        minimize_focused(s)
        three = open_panes(s)[0]["h"]

        check("three are put away", len(put_away(s)) == 3, str(here(s)))
        check("all three are listed", len(bar_entries(s)) == 3, str(bar_entries(s)))
        check(
            "and the survivor is no shorter than with one put away",
            three == one,
            f"{one} -> {three}",
        )
        check(
            "because the bar was one row either way", entries_one == 1, str(entries_one)
        )


def test_clicking_an_entry_brings_that_pane_back():
    with Session(SH, cols=100, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        layout_before = sorted((p["x"], p["w"]) for p in open_panes(s))

        minimize_focused(s)
        gone = put_away(s)[0]["id"]
        e = [h for h in bar_entries(s) if h["action"] == f"focus:{gone}"]
        check(
            "the entry names the pane it stands for", len(e) == 1, str(bar_entries(s))
        )
        if not e:
            return
        s.click(e[0]["x"] + 1, e[0]["y"])
        s.settle(20)
        check("clicking it restores the pane", put_away(s) == [], str(here(s)))
        check(
            "and the layout is the one it left",
            sorted((p["x"], p["w"]) for p in open_panes(s)) == layout_before,
            str(here(s)),
        )


def test_focusing_it_by_any_route_brings_it_back():
    """Restoring is not a special verb: a focused pane is never minimised, and
    that is checked once a frame rather than at each place focus can move."""
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        minimize_focused(s)
        gone = put_away(s)[0]["id"]

        s.api("focus", id=gone)
        s.settle(20)
        check(
            "focusing it over the control API restores it",
            put_away(s) == [],
            str(here(s)),
        )


def test_it_will_not_leave_the_tab_empty():
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        check("one pane to start", len(here(s)) == 1, str(here(s)))
        minimize_focused(s)
        check("minimising the only pane is refused", put_away(s) == [], str(here(s)))
        check(
            "and it says why",
            "nothing else" in s.snapshot().screen(),
            repr(s.snapshot().screen()[-200:]),
        )

        s.api("split", dir="cols")
        s.settle(20)
        minimize_focused(s)
        check("with two, one can go", len(put_away(s)) == 1, str(here(s)))
        minimize_focused(s)
        check("but the last one standing cannot", len(put_away(s)) == 1, str(here(s)))


def test_the_button_minimises_that_pane():
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        target = here(s)[0]["id"]
        b = [h for h in s.snapshot().hits if h["action"] == f"minimize:{target}"]
        check("a pane's frame carries a minimise button", len(b) == 1, str(b))
        if not b:
            return
        s.click(b[0]["x"] + 1, b[0]["y"])
        s.settle(20)
        check(
            "clicking it puts that pane away",
            [p["id"] for p in put_away(s)] == [target],
            str(here(s)),
        )


def test_it_has_no_say_once_the_tab_is_a_list():
    """Down there every pane is a row already, so being minimised is not a
    different thing to be -- which is why it costs nothing."""
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        minimize_focused(s)
        check("one put away, one open", len(put_away(s)) == 1, str(here(s)))

        s.resize(26, 20)
        s.settle(20)
        rows = here(s)
        check("the tab is a list", any(p["hidden"] for p in rows), str(rows))
        check(
            "and every pane is in it, minimised or not",
            all(p["w"] > 1 for p in rows),
            str(rows),
        )

        s.resize(90, 20)
        s.settle(20)
        check(
            "coming back, it is put away again and not lost",
            len(put_away(s)) == 1 and len(open_panes(s)) == 1,
            str(here(s)),
        )


def test_zoom_hides_the_bar():
    with Session(SH, cols=90, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.api("split", dir="rows")
        s.settle(20)
        minimize_focused(s)
        check("a bar to start", len(bar_entries(s)) == 1, str(bar_entries(s)))
        s.send(r"\x01z")
        s.settle(20)
        check(
            "zooming shows one pane and nothing else, bar included",
            bar_entries(s) == [],
            str(bar_entries(s)),
        )
        s.send(r"\x01z")
        s.settle(20)
        check(
            "and unzooming gives the bar back",
            len(bar_entries(s)) == 1,
            str(bar_entries(s)),
        )


if __name__ == "__main__":
    test_minimising_takes_a_pane_out_of_the_layout()
    test_the_bar_is_one_row_however_many_are_put_away()
    test_clicking_an_entry_brings_that_pane_back()
    test_focusing_it_by_any_route_brings_it_back()
    test_it_will_not_leave_the_tab_empty()
    test_the_button_minimises_that_pane()
    test_it_has_no_say_once_the_tab_is_a_list()
    test_zoom_hides_the_bar()
    sys.exit(report())
