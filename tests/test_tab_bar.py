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


def _status(s, text):
    s.send(rf"\e]5577;1;status;{text}\e\\")
    s.settle(30)


def test_status_rows_sit_under_their_tab():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        pane = s.pane(0)["id"]
        _status(s, "building 3/7")
        s.api("new-tab")
        s.settle(30)
        snap = s.snapshot()
        check(
            "a pane's OSC status is a row under its tab",
            "building 3/7" in snap.line(3)[:W],
            repr(snap.line(3)[:W]),
        )
        check(
            "the other tab moved down to make room",
            snap.hit_at(1, 4) == "tab:2",
            str(snap.hit_at(1, 4)),
        )
        check(
            "the row is a door to the pane that said it",
            snap.hit_at(2, 3) == f"find:{pane}",
            str(snap.hit_at(2, 3)),
        )
        # We are in tab 2: clicking the status row is a cross-tab jump.
        s.click(2, 3)
        s.until(lambda _: s.tabs()[0]["active"])
        check("clicking it selects that tab", s.tabs()[0]["active"], str(s.tabs()))
        check(
            "...and focuses that pane",
            s.focused()["id"] == pane,
            str(s.focused()["id"]),
        )


def test_status_cap_spends_its_last_row_on_the_ellipsis():
    capped = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_status 1\n')
    with Session(SH, cols=90, rows=20, config=capped) as s:
        s.settle(30)
        _status(s, "first")
        s.api("split", dir="cols")
        s.settle(30)
        _status(s, "second")
        snap = s.snapshot()
        check(
            "over the cap, the row says there was more",
            snap.line(3)[:W].strip() == "\u2026",
            repr(snap.line(3)[:W]),
        )
        check(
            "an ellipsis is not a door",
            snap.hit_at(2, 3) is None,
            str(snap.hit_at(2, 3)),
        )
        check(
            "and only the cap's rows were spent",
            snap.hit_at(1, 4) == "newtab",
            str(snap.hit_at(1, 4)),
        )


def test_a_long_status_is_cut_and_says_so():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        _status(s, "a status far too long for the sidebar")
        row = s.snapshot().line(3)[:W]
        check(
            "the status stops at the sidebar's edge", row.endswith("\u2026"), repr(row)
        )


def test_a_dead_pane_reports_its_exit_instead():
    # Wider than the default: "exited: status 3" plus the indent is 19 cells,
    # and the point here is the words, not the (already tested) truncation.
    kept = _cfg('tab_bar_side "left"\ntab_bar_width 24\nkeep_dead "all"\n')
    with Session(["/bin/sh", "-c", "exit 3"], cols=90, rows=20, config=kept) as s:
        s.until(lambda snap: "exited" in snap.line(3))
        row = s.snapshot().line(3)[:24]
        check("how it died replaces what it last said", "status 3" in row, repr(row))


def test_an_unnamed_tab_borrows_its_directory():
    cd = ["/bin/sh", "-c", "cd /tmp && stty raw -echo; cat"]
    with Session(cd, cols=90, rows=20, config=LEFT) as s:
        s.settle(60)
        row = s.snapshot().line(2)[:W]
        check("the label is the focused pane's dir", "1:tmp" in row, repr(row))


def test_a_real_name_beats_the_derived_one():
    lay = tempfile.NamedTemporaryFile("w", suffix=".layout", delete=False)
    lay.write('layout {\n tab name="api" {\n  pane\n }\n}\n')
    lay.close()
    with Session(SH, cols=90, rows=20, config=LEFT, layout=lay.name) as s:
        s.settle(60)
        row = s.snapshot().line(2)[:W]
        check("a declared name is not second-guessed", "1:api" in row, repr(row))


def test_the_borrowed_name_follows_a_cd():
    loop = ["/bin/sh", "-c", 'cd /tmp && while IFS= read -r l; do eval "$l"; done']
    with Session(loop, cols=90, rows=20, config=LEFT) as s:
        s.until(lambda snap: "1:tmp" in snap.line(2))
        s.send(r"cd /home\n")
        s.until(lambda snap: "1:home" in snap.line(2))
        row = s.snapshot().line(2)[:W]
        check("the kernel's answer moves the label", "1:home" in row, repr(row))


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
    test_status_rows_sit_under_their_tab()
    test_status_cap_spends_its_last_row_on_the_ellipsis()
    test_a_long_status_is_cut_and_says_so()
    test_a_dead_pane_reports_its_exit_instead()
    test_an_unnamed_tab_borrows_its_directory()
    test_a_real_name_beats_the_derived_one()
    test_the_borrowed_name_follows_a_cd()
    test_narrow_terminal_falls_back_to_top()
    test_growing_back_restores_the_sidebar()
    sys.exit(report())
