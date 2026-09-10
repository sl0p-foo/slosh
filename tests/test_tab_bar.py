#!/usr/bin/env python3
"""The tab bar on its side: tab_bar_side left / right.

A sidebar lists one tab per row down the chosen edge and the panes give up
its columns instead of the top row. The strip's indicators (pane count, the
prefix badge) move to the sidebar's bottom rows, the `+` sits on the row
after the last tab, and every mouse verb keeps working because the hits move
with the cells. A terminal too narrow to give up the columns falls back to
the top strip until it grows.
"""

import sys
import tempfile

from harness import Session, check, report


def _cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


W = 18
LEFT = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\n')
RIGHT = _cfg(f'tab_bar_side "right"\ntab_bar_width {W}\n')

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]


def test_left_sidebar_reserves_columns():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        snap = s.snapshot()
        # gap 1 + tab_bar_pad 1 (the defaults): the first tab is row 2.
        check(
            "the first tab is a row in the sidebar",
            snap.hit_at(1, 2) == "tab:1",
            str(snap.hit_at(1, 2)),
        )
        check(
            "the whole row is the target, not just the label",
            snap.hit_at(W - 1, 2) == "tab:1",
            str(snap.hit_at(W - 1, 2)),
        )
        check(
            "the + sits on the row after the last tab",
            snap.hit_at(1, 3) == "newtab",
            str(snap.hit_at(1, 3)),
        )
        p = s.pane(0)
        check("panes give up the sidebar's columns", p["x"] >= W, str(p["x"]))
        # rows 20, status line on 19: the count takes the sidebar's last row.
        check(
            "the pane count moved to the sidebar's bottom",
            "1 pane" in snap.line(18)[:W],
            repr(snap.line(18)[:W]),
        )


def test_sidebar_rows_click_like_the_strip():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        s.click(1, 3)  # the + row: a new tab, which becomes the active one
        s.until(lambda _: len(s.tabs()) == 2)
        check("the + row makes a tab", len(s.tabs()) == 2, str(len(s.tabs())))
        check("...and it is the one you are in", s.tabs()[1]["active"], str(s.tabs()))
        snap = s.snapshot()
        check(
            "the new tab is the next row down",
            snap.hit_at(1, 3) == "tab:2",
            str(snap.hit_at(1, 3)),
        )
        s.click(1, 2)  # the first tab's row
        s.until(lambda _: s.tabs()[0]["active"])
        check("clicking a row selects that tab", s.tabs()[0]["active"], str(s.tabs()))


def test_right_sidebar_takes_the_other_edge():
    with Session(SH, cols=90, rows=20, config=RIGHT) as s:
        s.settle(30)
        snap = s.snapshot()
        x0 = 90 - W
        check(
            "the first tab is a row on the right edge",
            snap.hit_at(x0 + 1, 2) == "tab:1",
            str(snap.hit_at(x0 + 1, 2)),
        )
        p = s.pane(0)
        check(
            "panes stop where the sidebar starts",
            p["x"] + p["w"] <= x0,
            f"{p['x']}+{p['w']} vs {x0}",
        )


def test_pad_pushes_the_list_down():
    pad = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_pad 3\n')
    with Session(SH, cols=90, rows=20, config=pad) as s:
        s.settle(30)
        snap = s.snapshot()
        # gap 1 + pad 3: the first tab is on row 4, and the rows above are air.
        check(
            "the pad rows are not targets",
            snap.hit_at(1, 1) is None and snap.hit_at(1, 3) is None,
            f"{snap.hit_at(1, 1)} / {snap.hit_at(1, 3)}",
        )
        check(
            "the first tab starts below the pad",
            snap.hit_at(1, 4) == "tab:1",
            str(snap.hit_at(1, 4)),
        )
        p = s.pane(0)
        check(
            "the pad is paint, not layout: panes keep their rows",
            p["y"] <= 2,
            str(p["y"]),
        )


def test_narrow_terminal_falls_back_to_top():
    # 40 < width + min_pane cols + 4: the sidebar would leave no room for the
    # pane it is chrome for, so the strip goes back to the top row.
    with Session(SH, cols=40, rows=20, config=LEFT) as s:
        s.settle(30)
        snap = s.snapshot()
        check(
            "the strip is a top row again",
            snap.hit_at(5, 1) == "tab:1",
            str(snap.hit_at(5, 1)),
        )
        p = s.pane(0)
        check("and the panes keep the columns", p["x"] < W, str(p["x"]))


def test_growing_back_restores_the_sidebar():
    with Session(SH, cols=40, rows=20, config=LEFT) as s:
        s.settle(30)
        s.resize(90, 20)
        s.until(lambda snap: snap.hit_at(1, 2) == "tab:1")
        check(
            "a resize moves the bar without anyone storing state",
            s.snapshot().hit_at(1, 2) == "tab:1",
            str(s.snapshot().hit_at(1, 2)),
        )
        p = s.pane(0)
        check("and the panes give the columns back", p["x"] >= W, str(p["x"]))


if __name__ == "__main__":
    test_left_sidebar_reserves_columns()
    test_sidebar_rows_click_like_the_strip()
    test_right_sidebar_takes_the_other_edge()
    test_pad_pushes_the_list_down()
    test_narrow_terminal_falls_back_to_top()
    test_growing_back_restores_the_sidebar()
    sys.exit(report())
