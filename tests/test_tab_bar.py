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
BARE = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_chrome false\n')

# With tab_bar_chrome (the default) the frame takes a column each side and a
# row top and bottom: content runs x 1..16, and gap 1 + frame 1 + pad 0 puts
# the first tab on row 2. CX/CW/Y0 are that content box, so a geometry change
# is one edit here rather than thirty coordinates below.
CX, CW, Y0 = 1, W - 2, 2

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]


def _row(snap, y):
    """The sidebar's content on a row, without the frame either side."""
    return snap.line(y)[CX : CX + CW]


def test_left_sidebar_reserves_columns():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        snap = s.snapshot()
        check(
            "the first tab is a row in the sidebar",
            snap.hit_at(CX, Y0) == "tab:1",
            str(snap.hit_at(CX, Y0)),
        )
        check(
            "the whole row is the target, not just the label",
            snap.hit_at(CX + CW - 1, Y0) == "tab:1",
            str(snap.hit_at(CX + CW - 1, Y0)),
        )
        check(
            "the + sits on the row after the last tab",
            snap.hit_at(CX, Y0 + 1) == "newtab",
            str(snap.hit_at(CX, Y0 + 1)),
        )
        p = s.pane(0)
        check("panes give up the sidebar's columns", p["x"] >= W, str(p["x"]))
        # rows 20, status line on 19, the frame's bottom line on 18.
        check(
            "the pane count moved to the sidebar's bottom",
            "1 pane" in _row(snap, 17),
            repr(_row(snap, 17)),
        )


def test_sidebar_rows_click_like_the_strip():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        s.click(CX, Y0 + 1)  # the + row: a new tab, which becomes the active one
        s.until(lambda _: len(s.tabs()) == 2)
        check("the + row makes a tab", len(s.tabs()) == 2, str(len(s.tabs())))
        check("...and it is the one you are in", s.tabs()[1]["active"], str(s.tabs()))
        snap = s.snapshot()
        check(
            "the new tab is the next row down",
            snap.hit_at(CX, Y0 + 1) == "tab:2",
            str(snap.hit_at(CX, Y0 + 1)),
        )
        s.click(CX, Y0)  # the first tab's row
        s.until(lambda _: s.tabs()[0]["active"])
        check("clicking a row selects that tab", s.tabs()[0]["active"], str(s.tabs()))


def test_right_sidebar_takes_the_other_edge():
    with Session(SH, cols=90, rows=20, config=RIGHT) as s:
        s.settle(30)
        snap = s.snapshot()
        x0 = 90 - W
        check(
            "the first tab is a row on the right edge",
            snap.hit_at(x0 + CX, Y0) == "tab:1",
            str(snap.hit_at(x0 + CX, Y0)),
        )
        p = s.pane(0)
        check(
            "panes stop where the sidebar starts",
            p["x"] + p["w"] <= x0,
            f"{p['x']}+{p['w']} vs {x0}",
        )


def test_pad_pushes_the_list_down():
    # The default pad is 0, so Y0 is the unpadded first row and a pad of N
    # moves the list exactly N rows down it.
    n = 3
    pad = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_pad {n}\n')
    with Session(SH, cols=90, rows=20, config=pad) as s:
        s.settle(30)
        snap = s.snapshot()
        check(
            "the pad rows are not targets",
            all(snap.hit_at(CX, Y0 + k) is None for k in range(n)),
            str([snap.hit_at(CX, Y0 + k) for k in range(n)]),
        )
        check(
            "the first tab starts below the pad",
            snap.hit_at(CX, Y0 + n) == "tab:1",
            str(snap.hit_at(CX, Y0 + n)),
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
            "building 3/7" in _row(snap, Y0 + 1),
            repr(_row(snap, Y0 + 1)),
        )
        check(
            "the other tab moved down to make room",
            snap.hit_at(CX, Y0 + 2) == "tab:2",
            str(snap.hit_at(CX, Y0 + 2)),
        )
        check(
            "the row is a door to the pane that said it",
            snap.hit_at(CX + 1, Y0 + 1) == f"find:{pane}",
            str(snap.hit_at(CX + 1, Y0 + 1)),
        )
        # We are in tab 2: clicking the status row is a cross-tab jump.
        s.click(CX + 1, Y0 + 1)
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
            _row(snap, Y0 + 1).strip() == "\u2026",
            repr(_row(snap, Y0 + 1)),
        )
        check(
            "an ellipsis is not a door",
            snap.hit_at(CX + 1, Y0 + 1) is None,
            str(snap.hit_at(CX + 1, Y0 + 1)),
        )
        check(
            "and only the cap's rows were spent",
            snap.hit_at(CX, Y0 + 2) == "newtab",
            str(snap.hit_at(CX, Y0 + 2)),
        )


def test_a_long_status_is_cut_and_says_so():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        _status(s, "a status far too long for the sidebar")
        row = _row(s.snapshot(), Y0 + 1)
        check(
            "the status stops at the sidebar's edge", row.endswith("\u2026"), repr(row)
        )


def test_a_dead_pane_reports_its_exit_instead():
    # Wider than the default: "exited: status 3" plus the indent is 19 cells,
    # and the point here is the words, not the (already tested) truncation.
    kept = _cfg('tab_bar_side "left"\ntab_bar_width 24\nkeep_dead "all"\n')
    with Session(["/bin/sh", "-c", "exit 3"], cols=90, rows=20, config=kept) as s:
        s.until(lambda snap: "exited" in snap.line(Y0 + 1))
        row = s.snapshot().line(Y0 + 1)[CX : CX + 22]
        check("how it died replaces what it last said", "status 3" in row, repr(row))


def test_an_unnamed_tab_borrows_its_directory():
    cd = ["/bin/sh", "-c", "cd /tmp && stty raw -echo; cat"]
    with Session(cd, cols=90, rows=20, config=LEFT) as s:
        s.settle(60)
        row = _row(s.snapshot(), Y0)
        check("the label is the focused pane's dir", "1:tmp" in row, repr(row))


def test_a_real_name_beats_the_derived_one():
    lay = tempfile.NamedTemporaryFile("w", suffix=".layout", delete=False)
    lay.write('layout {\n tab name="api" {\n  pane\n }\n}\n')
    lay.close()
    with Session(SH, cols=90, rows=20, config=LEFT, layout=lay.name) as s:
        s.settle(60)
        row = _row(s.snapshot(), Y0)
        check("a declared name is not second-guessed", "1:api" in row, repr(row))


def test_the_borrowed_name_follows_a_cd():
    loop = ["/bin/sh", "-c", 'cd /tmp && while IFS= read -r l; do eval "$l"; done']
    with Session(loop, cols=90, rows=20, config=LEFT) as s:
        s.until(lambda snap: "1:tmp" in snap.line(Y0))
        s.send(r"cd /home\n")
        s.until(lambda snap: "1:home" in snap.line(Y0))
        row = _row(s.snapshot(), Y0)
        check("the kernel's answer moves the label", "1:home" in row, repr(row))


def test_the_sidebar_is_framed_like_a_pane():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(40)
        snap = s.snapshot()
        top, bottom = snap.line(1), snap.line(18)
        check("a frame runs round the list", top[0] in "\u256d\u250c", repr(top[:W]))
        check(
            "...closed at the bottom, above the status line",
            bottom[0] in "\u2570\u2514",
            repr(bottom[:W]),
        )
        check(
            "and the list sits inside it",
            snap.line(Y0)[0] == "\u2502" and snap.line(Y0)[CX + CW] == "\u2502",
            repr(snap.line(Y0)[:W]),
        )


def test_compact_shares_its_lines_with_the_ring():
    """The point of the chrome: one figure, not a box beside a box.

    The sidebar's inner vertical *is* the tab area's ring column, so the
    stroke union turns the ring's top-left corner into a tee -- nobody
    computes that, it falls out of the same machinery the dividers use.
    """
    conf = _cfg(f'compact true\ntab_bar_side "left"\ntab_bar_width {W}\n')
    with Session(SH, cols=90, rows=20, config=conf) as s:
        s.settle(40)
        snap = s.snapshot()
        check(
            "the sidebar meets the ring in a junction, not a corner",
            snap.line(0)[W] == "\u252c",
            repr(snap.line(0)[: W + 2]),
        )
        check(
            "...and again at the bottom",
            snap.line(18)[W] == "\u2534",
            repr(snap.line(18)[: W + 2]),
        )


def test_chrome_false_gives_the_columns_back():
    with Session(SH, cols=90, rows=20, config=BARE) as s:
        s.settle(40)
        snap = s.snapshot()
        # No frame row either, so the list starts on the gap's own row.
        check(
            "no frame: the list starts in the first column",
            snap.hit_at(0, 1) == "tab:1",
            str(snap.hit_at(0, 1)),
        )
        check(
            "and the whole width is the target",
            snap.hit_at(W - 1, 1) == "tab:1",
            str(snap.hit_at(W - 1, 1)),
        )


def test_the_sidebar_button_says_what_it_does():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(40)
        snap = s.snapshot()
        row = _row(snap, Y0 + 1)
        check("a row of its own spells the verb out", "new tab" in row, repr(row))
        check(
            "and the whole row is the target, like a tab's",
            snap.hit_at(CX + CW - 1, Y0 + 1) == "newtab",
            str(snap.hit_at(CX + CW - 1, Y0 + 1)),
        )


def test_a_narrow_sidebar_keeps_the_bare_mark():
    narrow = _cfg('tab_bar_side "left"\ntab_bar_width 10\n')
    with Session(SH, cols=90, rows=20, config=narrow) as s:
        s.settle(40)
        snap = s.snapshot()
        row = snap.line(Y0 + 1)[CX : CX + 8]
        check(
            "no room for the word, so it is not said", "new tab" not in row, repr(row)
        )
        check(
            "...but the button is still there",
            snap.hit_at(CX, Y0 + 1) == "newtab",
            str(snap.hit_at(CX, Y0 + 1)),
        )


def test_the_strip_keeps_its_bare_mark():
    """The word is a sidebar's affordance, not a new spelling everywhere: in
    the strip the mark sits at the end of a row of tabs, where columns are
    scarce and the company it keeps says what it is."""
    with Session(SH, cols=90, rows=20) as s:
        s.settle(40)
        check(
            "the strip is unchanged",
            "new tab" not in s.snapshot().line(1),
            repr(s.snapshot().line(1)),
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
        s.until(lambda snap: snap.hit_at(CX, Y0) == "tab:1")
        check(
            "a resize moves the bar without anyone storing state",
            s.snapshot().hit_at(CX, Y0) == "tab:1",
            str(s.snapshot().hit_at(CX, Y0)),
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
    test_the_sidebar_is_framed_like_a_pane()
    test_compact_shares_its_lines_with_the_ring()
    test_chrome_false_gives_the_columns_back()
    test_the_sidebar_button_says_what_it_does()
    test_a_narrow_sidebar_keeps_the_bare_mark()
    test_the_strip_keeps_its_bare_mark()
    test_narrow_terminal_falls_back_to_top()
    test_growing_back_restores_the_sidebar()
    sys.exit(report())
