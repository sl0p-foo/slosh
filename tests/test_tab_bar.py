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

# The default newtab_pad: one blank row between the last tab and the `+`, so
# a pointer one row low misses the button instead of making a tab.
NT_PAD = 1

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
            "the + sits below the last tab, a pad row clear of it",
            snap.hit_at(CX, Y0 + 1 + NT_PAD) == "newtab",
            str(snap.hit_at(CX, Y0 + 1 + NT_PAD)),
        )
        check(
            "and the air between them is nobody's target",
            snap.hit_at(CX, Y0 + 1) is None,
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
        # the + row: a new tab, which becomes the active one
        s.click(CX, Y0 + 1 + NT_PAD)
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
    """The focused pane announces a status, the way a program in it would.

    `raw`, not `send`. They are different directions: `raw` writes the pane's
    pty, where `cat` echoes the bytes back and the pane's own terminal parses
    them -- the path a program's output takes. `send` feeds slosh's input
    decoder instead, which is the path a keystroke from the outer terminal
    takes, and an OSC arriving there is a reply to slosh, not output from the
    program. Sent that way the sequence never reaches the pane at all: it is
    swallowed by the decoder, the status stays empty, and every check below
    fails against the *next tab's label* on the row it expected a status on.
    """
    s.raw(rf"\e]5577;1;status;{text}\e\\")
    s.settle(30)


# The sidebar's default. LEFT would do as well -- the point of these is the
# alignment, so they say which mode they mean rather than leaning on it.
RIGHT_IDX = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_index "right"\n')
PREFIX_IDX = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_index "prefix"\n')


def test_the_index_sits_at_the_far_end():
    """The sidebar's default: names all start in one column, numbers line up in
    another. A sidebar row is wider than the name on it, and a prefix spends
    the start of every row on a number nobody reads until they want to press
    it."""
    with Session(SH, cols=90, rows=20, config=RIGHT_IDX) as s:
        s.settle(30)
        s.api("new-tab", name="api")
        s.settle(30)
        rows = [_row(snap := s.snapshot(), Y0), _row(snap, Y0 + 1)]
        check(
            "the number is against the right edge",
            [r.rstrip()[-1] for r in rows] == ["1", "2"],
            str(rows),
        )
        check(
            "...and every name starts in the same column",
            rows[1].index("api") == rows[0].index("tests"),
            str(rows),
        )
        check("no prefix is left behind", "1:" not in rows[0], str(rows))


def test_a_long_name_stops_short_of_the_number():
    """Otherwise `verylongproje2` is a tab called verylongproje2, and nothing
    on the row says otherwise."""
    with Session(SH, cols=90, rows=20, config=RIGHT_IDX) as s:
        s.settle(30)
        s.api("new-tab", name="verylongprojectname")
        s.settle(30)
        row = _row(s.snapshot(), Y0 + 1)
        check("the number survived the long name", row.rstrip()[-1] == "2", repr(row))
        check(
            "...with a cell of air before it",
            row.rstrip()[-2] == " ",
            repr(row),
        )
        check("and the row is still one plate wide", len(row) == CW, repr(row))


def test_a_narrow_row_keeps_the_prefix():
    """The name is the part you came for: at eight columns there is nothing to
    align and the number goes back in front, where it costs two cells."""
    narrow = _cfg('tab_bar_side "left"\ntab_bar_width 8\ntab_bar_index "right"\n')
    with Session(SH, cols=90, rows=20, config=narrow) as s:
        s.settle(30)
        row = s.snapshot().line(Y0)[1:7]
        check("the prefix is back", "1:" in row, repr(row))


def test_the_top_strip_ignores_it():
    """A strip tab is as wide as its label, so there is no far end to align
    against -- and a number floating after a name it does not belong to would
    be worse than the prefix it replaced."""
    top = _cfg('tab_bar_index "right"\n')
    with Session(SH, cols=60, rows=10, config=top) as s:
        s.settle(30)
        check(
            "the strip still writes 1:name",
            "1:tests" in s.snapshot().screen(),
            s.snapshot().line(1),
        )


def test_the_whole_row_is_still_one_door():
    with Session(SH, cols=90, rows=20, config=RIGHT_IDX) as s:
        s.settle(30)
        s.api("new-tab", name="api")
        s.settle(30)
        snap = s.snapshot()
        at_name = snap.hit_at(CX + 1, Y0 + 1)
        at_number = snap.hit_at(CX + CW - 2, Y0 + 1)
        check(
            "clicking the number is clicking the tab",
            at_name and at_name == at_number,
            f"{at_name} vs {at_number}",
        )
        st = snap.style_at(CX + CW - 2, Y0 + 1)
        check(
            "and the number sits on the tab's own plate",
            st and st["bg"] == (snap.style_at(CX + 1, Y0 + 1) or {}).get("bg"),
            str(st),
        )


def test_the_prefix_is_still_there_for_anyone_who_wants_it():
    """`1:name`, which is what the top strip draws and what the sidebar drew
    before the alignment existed."""
    with Session(SH, cols=90, rows=20, config=PREFIX_IDX) as s:
        s.settle(30)
        row = _row(s.snapshot(), Y0)
        check("the number is in front of the name", row.strip() == "1:tests", repr(row))


def test_the_sidebar_aligns_by_default():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        row = _row(s.snapshot(), Y0)
        check("no config asked for it", row.rstrip()[-1] == "1", repr(row))
        check("...and no prefix came with it", "1:" not in row, repr(row))


def test_a_bad_index_mode_is_a_line_and_no_more():
    bad = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_index "middle"\n')
    with Session(SH, cols=90, rows=20, config=bad) as s:
        s.settle(30)
        row = _row(s.snapshot(), Y0)
        check("the session runs, with the default", row.rstrip()[-1] == "1", repr(row))


def test_every_tab_sits_on_a_plate():
    """The tab you are in is the accent; the others are `tab_idle_bg`. The
    quiet fill is what gives a tab edges -- and what the active fill is
    brighter than."""
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        s.api("new-tab")
        s.settle(30)
        snap = s.snapshot()
        idle, active = snap.style_at(CX, Y0), snap.style_at(CX, Y0 + 1)
        check("the tab you are not in is filled", idle and idle["bg"], str(idle))
        check(
            "...in something other than the active tab's fill",
            active and active["bg"] != idle["bg"],
            f"{idle} vs {active}",
        )
        end = snap.style_at(CX + CW - 1, Y0)
        check(
            "and the plate is the whole row, which is the whole hit",
            end and end["bg"] == idle["bg"],
            str(end),
        )


def test_an_old_theme_gets_an_idle_plate_in_its_own_family():
    """Same bargain as the status colours: mixed from two colours the theme
    does define rather than one stock colour dropped into its palette."""
    bg, idle = "#101020", "#88ff88"
    old = _cfg(
        f'tab_bar_side "left"\ntab_bar_width {W}\n'
        f'theme {{ default_bg "{bg}"\n tab_idle "{idle}" }}\n'
    )
    with Session(SH, cols=90, rows=20, config=old) as s:
        s.settle(30)
        s.api("new-tab")
        s.settle(30)
        st = s.snapshot().style_at(CX, Y0)
        got, a, b = _rgb(st["bg"]), _rgb(bg), _rgb(idle)
        check(
            "the plate is mixed from the theme's own background and label grey",
            all(min(a[i], b[i]) <= got[i] <= max(a[i], b[i]) for i in range(3)),
            f"{st['bg']} not between {bg} and {idle}",
        )
        check(
            "...and nearer the background than the label",
            sum(abs(got[i] - a[i]) for i in range(3))
            < sum(abs(got[i] - b[i]) for i in range(3)),
            st["bg"],
        )


def test_a_theme_can_have_the_bare_tabs_back():
    """Naming the background is how you say "no plate" -- the knob wins over
    the derivation, as every named colour does."""
    bare = _cfg(
        f'tab_bar_side "left"\ntab_bar_width {W}\n'
        'theme { default_bg "#101020"\n tab_idle_bg "#101020" }\n'
    )
    with Session(SH, cols=90, rows=20, config=bare) as s:
        s.settle(30)
        s.api("new-tab")
        s.settle(30)
        st = s.snapshot().style_at(CX, Y0)
        check("the named colour is the one drawn", st["bg"] == "#101020", str(st))


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


def test_status_rows_are_told_apart_by_the_slant_not_by_an_indent():
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        _status(s, "building 3/7")
        snap = s.snapshot()
        label, status = _row(snap, Y0), _row(snap, Y0 + 1)
        # One leading space each: a status starts in the column its tab's name
        # starts in. Three columns of indent is a fifth of this sidebar.
        check(
            "a status is not indented under its label",
            status.index("building") == label.index("tests"),
            f"{status.index('building')} vs {label.index('tests')}",
        )
        st = snap.style_at(CX + 1, Y0 + 1)
        check("it is italic instead", "italic" in st["attrs"], str(st))
        check(
            "...and the label above it is not",
            "italic" not in snap.style_at(CX + 1, Y0)["attrs"],
            str(snap.style_at(CX + 1, Y0)),
        )


def test_status_rows_take_their_own_theme_colours():
    themed = _cfg(
        f'tab_bar_side "left"\ntab_bar_width {W}\n'
        'theme { tab_status_fg "#00ff88"\n tab_status_bg "#202030" }\n'
    )
    with Session(SH, cols=90, rows=20, config=themed) as s:
        s.settle(30)
        _status(s, "building")
        snap = s.snapshot()
        st = snap.style_at(CX + 1, Y0 + 1)
        check("the fg is the themed one", st["fg"] == "#00ff88", str(st))
        end = snap.style_at(CX + CW - 1, Y0 + 1)
        check(
            "and the band covers the row, not just the words",
            end and end["bg"] == "#202030",
            str(end),
        )


def _rgb(hexs):
    return tuple(int(hexs[i : i + 2], 16) for i in (1, 3, 5))


def test_an_old_theme_gets_coherent_status_colours_anyway():
    """A theme written before these rows existed names its own chrome and
    nothing about them. Mixing two colours it *does* define beats dropping one
    stock colour into somebody else's palette."""
    count, hover = "#ff8800", "#0000ff"
    old = _cfg(
        f'tab_bar_side "left"\ntab_bar_width {W}\n'
        f'theme {{ tab_count "{count}"\n tab_hover "{hover}" }}\n'
    )
    with Session(SH, cols=90, rows=20, config=old) as s:
        s.settle(30)
        _status(s, "building")
        st = s.snapshot().style_at(CX + 1, Y0 + 1)
        got, a, b = _rgb(st["fg"]), _rgb(count), _rgb(hover)
        check(
            "the status is mixed from the theme's own two colours",
            all(min(a[i], b[i]) <= got[i] <= max(a[i], b[i]) for i in range(3)),
            f"{st['fg']} not between {count} and {hover}",
        )
        check(
            "...and is neither of them outright",
            st["fg"] not in (count, hover),
            st["fg"],
        )


def test_a_monochrome_theme_stays_monochrome():
    """The reason the distinction is a slant and the colour is mixed rather
    than picked: mono has no colour to spend, and must not be given any."""
    mono = _cfg(
        f'tab_bar_side "left"\ntab_bar_width {W}\n'
        'theme { tab_count "#585858"\n tab_hover "#e4e4e4" }\n'
    )
    with Session(SH, cols=90, rows=20, config=mono) as s:
        s.settle(30)
        _status(s, "building")
        st = s.snapshot().style_at(CX + 1, Y0 + 1)
        r, g, b = _rgb(st["fg"])
        check("no hue arrived from anywhere", r == g == b, st["fg"])
        check("but it is still italic", "italic" in st["attrs"], str(st))


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
            snap.hit_at(CX, Y0 + 2 + NT_PAD) == "newtab",
            str(snap.hit_at(CX, Y0 + 2 + NT_PAD)),
        )


def test_tab_gap_is_air_between_tabs_and_nothing_else():
    """Tabs each carrying a paragraph of status read as one column of text;
    the gap is what makes them separate entries again. It belongs *between*
    tabs: the space above the first is tab_bar_pad's and the space before the
    button is newtab_pad's, and neither should be paid twice."""
    n = 2
    gapped = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\ntab_gap {n}\n')
    with Session(SH, cols=90, rows=20, config=gapped) as s:
        s.settle(30)
        for _ in range(2):
            s.api("new-tab")
            s.settle(20)
        snap = s.snapshot()
        check(
            "the first tab still starts where it always did",
            snap.hit_at(CX, Y0) == "tab:1",
            str(snap.hit_at(CX, Y0)),
        )
        check(
            "the next one is that many rows lower",
            snap.hit_at(CX, Y0 + 1 + n) == "tab:2",
            str([snap.hit_at(CX, Y0 + 1 + k) for k in range(n + 1)]),
        )
        check(
            "and the air between is nobody's target",
            all(snap.hit_at(CX, Y0 + 1 + k) is None for k in range(n)),
            str([snap.hit_at(CX, Y0 + 1 + k) for k in range(n)]),
        )
        # Third tab, then the button: the gap is not paid again before the `+`.
        third = Y0 + 2 * (1 + n)
        check(
            "the button keeps its own pad, not the gap",
            snap.hit_at(CX, third + 1 + NT_PAD) == "newtab",
            str(snap.hit_at(CX, third + 1 + NT_PAD)),
        )
        p = s.pane(0)
        check(
            "the gap is paint, not layout: panes keep their rows",
            p["y"] <= 2,
            str(p["y"]),
        )


def test_tab_gap_gives_way_before_a_tab_does():
    """Spacing must never be what costs you the thing it spaces: a gap too
    big for the rows available shrinks, one row at a time, rather than
    pushing tabs off the bottom."""
    huge = _cfg(
        f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_status 0\ntab_gap 4\n'
    )
    with Session(SH, cols=90, rows=12, config=huge) as s:
        s.settle(30)
        for _ in range(4):
            s.api("new-tab")
            s.settle(20)
        snap = s.snapshot()
        targets = {h["action"] for h in snap.hits}
        missing = [t["index"] for t in s.tabs() if f"tab:{t['index']}" not in targets]
        check("every tab is still clickable", not missing, str(missing))
        check("and the button survived too", "newtab" in targets, str(sorted(targets)))


def test_statuses_never_cost_a_tab_its_row():
    """The list is the navigation; a status is an annotation on it.

    Rows used to be spent in tab order until they ran out, so talkative panes
    early in the list simply consumed the rows later tabs needed: four tabs of
    three announcing panes in a short terminal left the fourth with no row and
    no hit at all -- not clipped, unreachable -- and took the new-tab button
    with it. What gives way now is the annotation."""
    cfg = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_status 3\n')
    with Session(SH, cols=90, rows=16, config=cfg) as s:
        s.settle(30)
        for t in range(4):
            if t:
                s.api("new-tab")
                s.settle(20)
            _status(s, f"tab{t + 1} pane1")
            for p in (2, 3):
                s.api("split", dir="rows")
                s.settle(20)
                _status(s, f"tab{t + 1} pane{p}")

        snap = s.snapshot()
        targets = {h["action"] for h in snap.hits}
        missing = [t["index"] for t in s.tabs() if f"tab:{t['index']}" not in targets]
        check("every tab that exists can be clicked", not missing, str(missing))
        check(
            "the button was not crowded out either",
            "newtab" in targets,
            str(sorted(targets)),
        )
        # The annotation is what gave way: with four labels and a button to
        # fit into sixteen rows there is no room for twelve statuses, and it
        # is the last tabs' statuses that are missing rather than their rows.
        # The sidebar's own columns only: the panes' frames carry these same
        # words (a pane draws its status too), and would match anywhere.
        bar = [_row(snap, y) for y in range(16)]
        check(
            "the statuses are what went",
            not any("tab4" in r for r in bar),
            repr([r for r in bar if r.strip()]),
        )
        check(
            "...and the ones that fit are still there",
            any("tab1 pane1" in r for r in bar),
            repr([r for r in bar if r.strip()]),
        )


def _two_talking(s, first, second):
    """Two panes in one tab, each announcing. Returns their ids in row order."""
    a = s.pane(0)["id"]
    _status(s, first)
    s.api("split", dir="rows")
    s.settle(25)
    b = s.focused()["id"]
    _status(s, second)
    return a, b


LINES = _cfg(
    f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_status 8\n'
    "tab_bar_status_lines 3\n"
)


def test_a_status_can_be_given_more_than_one_row():
    """Sixteen cells is not enough for a sentence, and a sentence is what a
    pane has to say. Wrapped, the same column carries three times as much."""
    with Session(SH, cols=90, rows=20, config=LINES) as s:
        s.settle(30)
        _status(s, "add a summary row to the tab bar")
        snap = s.snapshot()
        rows = [_row(snap, Y0 + 1 + k).strip() for k in range(3)]
        check(
            "the words carry on down the column",
            " ".join(r for r in rows if r) == "add a summary row to the tab bar",
            repr(rows),
        )
        check(
            "and it broke at spaces, not mid-word",
            all(not r.endswith("\u2026") for r in rows if r),
            repr(rows),
        )


def test_one_line_still_cuts_at_the_column_not_the_word():
    """The default is unchanged. With nowhere for the word to go, stopping
    early would spend cells on nothing -- the reason to prefer a word boundary
    only exists once there is a next row to move the word to."""
    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        _status(s, "unwrapped statuses fill the column")
        row = _row(s.snapshot(), Y0 + 1)
        check("it is one row", not _row(s.snapshot(), Y0 + 2).strip(), repr(row))
        check("cut with an ellipsis", row.rstrip().endswith("\u2026"), repr(row))
        check(
            "and it used the whole column",
            len(row.rstrip()) >= CW - 1,
            f"{len(row.rstrip())} of {CW}",
        )


def test_wrapped_statuses_are_striped_so_panes_stay_apart():
    """Wrapped, the rows under a tab are a paragraph per pane rather than a
    line per pane, and the shape no longer says where one ends. The band does.
    At one line there is nothing to tell apart, so nothing is painted and a
    translucent terminal keeps its own background."""
    with Session(SH, cols=90, rows=20, config=LINES) as s:
        s.settle(30)
        _two_talking(s, "first pane says a fairly long thing", "second pane also")
        snap = s.snapshot()
        bgs = [snap.style_at(CX + 1, Y0 + 1 + k)["bg"] for k in range(4)]
        check("the first pane's rows share one background", bgs[0] == bgs[1], str(bgs))
        check("the next pane's is a different one", bgs[3] != bgs[0], str(bgs))

    with Session(SH, cols=90, rows=20, config=LEFT) as s:
        s.settle(30)
        _two_talking(s, "first", "second")
        snap = s.snapshot()
        check(
            "unwrapped, no band is painted at all",
            snap.style_at(CX + 1, Y0 + 1)["bg"] is None
            and snap.style_at(CX + 1, Y0 + 2)["bg"] is None,
            str([snap.style_at(CX + 1, Y0 + 1 + k)["bg"] for k in range(2)]),
        )


def test_every_row_of_a_status_is_the_same_door():
    """The rows are one status, so they are one target: clicking the second
    line of a sentence means the pane that said the sentence."""
    with Session(SH, cols=90, rows=20, config=LINES) as s:
        s.settle(30)
        pane = s.pane(0)["id"]
        _status(s, "a status long enough to need three whole rows of the bar")
        snap = s.snapshot()
        hits = [snap.hit_at(CX + 1, Y0 + 1 + k) for k in range(3)]
        check(
            "every row points at the pane that said it",
            hits == [f"find:{pane}"] * 3,
            str(hits),
        )


def test_wrapping_still_says_when_a_status_did_not_fit():
    """The allowance counts rows, so a wrapped status eats several of them.
    What does not fit is still reported rather than silently dropped."""
    tight = _cfg(
        f'tab_bar_side "left"\ntab_bar_width {W}\ntab_bar_status 4\n'
        "tab_bar_status_lines 3\n"
    )
    with Session(SH, cols=90, rows=20, config=tight) as s:
        s.settle(30)
        # The second needs more than the one row left over, so it is dropped
        # whole: half a wrapped sentence with no mark would read as all of it.
        _two_talking(
            s,
            "first pane says a fairly long thing here",
            "second pane also says a good deal for itself",
        )
        snap = s.snapshot()
        check(
            "the row after the first status says there was more",
            _row(snap, Y0 + 4).strip() == "\u2026",
            repr([_row(snap, Y0 + 1 + k) for k in range(5)]),
        )
        check(
            "an ellipsis is still not a door",
            snap.hit_at(CX + 1, Y0 + 4) is None,
            str(snap.hit_at(CX + 1, Y0 + 4)),
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
        check(
            "the label is the focused pane's dir",
            row.split() == ["tmp", "1"],
            repr(row),
        )


def test_a_real_name_beats_the_derived_one():
    lay = tempfile.NamedTemporaryFile("w", suffix=".layout", delete=False)
    lay.write('layout {\n tab name="api" {\n  pane\n }\n}\n')
    lay.close()
    with Session(SH, cols=90, rows=20, config=LEFT, layout=lay.name) as s:
        s.settle(60)
        row = _row(s.snapshot(), Y0)
        check(
            "a declared name is not second-guessed",
            row.split() == ["api", "1"],
            repr(row),
        )


def test_the_borrowed_name_follows_a_cd():
    loop = ["/bin/sh", "-c", 'cd /tmp && while IFS= read -r l; do eval "$l"; done']
    with Session(loop, cols=90, rows=20, config=LEFT) as s:
        s.until(lambda snap: "tmp" in snap.line(Y0))
        s.send(r"cd /home\n")
        s.until(lambda snap: "home" in snap.line(Y0))
        row = _row(s.snapshot(), Y0)
        check(
            "the kernel's answer moves the label",
            row.split() == ["home", "1"],
            repr(row),
        )


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
        row = _row(snap, Y0 + 1 + NT_PAD)
        check("a row of its own spells the verb out", "new tab" in row, repr(row))
        check(
            "and the whole row is the target, like a tab's",
            snap.hit_at(CX + CW - 1, Y0 + 1 + NT_PAD) == "newtab",
            str(snap.hit_at(CX + CW - 1, Y0 + 1 + NT_PAD)),
        )


def test_newtab_pad_is_the_air_before_the_button():
    """The button is the one row in the list that does not mean "go here", and
    a sidebar tab is a target the full width of the strip: flush against the
    tabs, a pointer one row low makes a tab instead of switching to one. The
    pad is that air, and 0 gives the old flush button back."""
    n = 3
    padded = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\nnewtab_pad {n}\n')
    with Session(SH, cols=90, rows=20, config=padded) as s:
        s.settle(30)
        snap = s.snapshot()
        check(
            "the air is not a target",
            all(snap.hit_at(CX, Y0 + 1 + k) is None for k in range(n)),
            str([snap.hit_at(CX, Y0 + 1 + k) for k in range(n)]),
        )
        check(
            "the button is that many rows down",
            snap.hit_at(CX, Y0 + 1 + n) == "newtab",
            str(snap.hit_at(CX, Y0 + 1 + n)),
        )
        p = s.pane(0)
        check(
            "the pad is paint, not layout: panes keep their rows",
            p["y"] <= 2,
            str(p["y"]),
        )

    flush = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\nnewtab_pad 0\n')
    with Session(SH, cols=90, rows=20, config=flush) as s:
        s.settle(30)
        snap = s.snapshot()
        check(
            "0 puts it straight under the last tab",
            snap.hit_at(CX, Y0 + 1) == "newtab",
            str(snap.hit_at(CX, Y0 + 1)),
        )


def test_newtab_button_false_removes_it_everywhere():
    """The verb keeps working from the keyboard; only the place to click it
    goes. A sidebar that is missing it has no `newtab` hit anywhere on it --
    including as somewhere to drop a pane."""
    off = _cfg(f'tab_bar_side "left"\ntab_bar_width {W}\nnewtab_button false\n')
    with Session(SH, cols=90, rows=20, config=off) as s:
        s.settle(30)
        snap = s.snapshot()
        check(
            "no button on the sidebar",
            all(h["action"] != "newtab" for h in snap.hits),
            str([h for h in snap.hits if h["action"] == "newtab"]),
        )
        check(
            "nor the word that spells it out",
            all("new tab" not in snap.line(y) for y in range(20)),
            repr([snap.line(y) for y in range(20) if "new tab" in snap.line(y)]),
        )
        s.api("new-tab")
        s.until(lambda _: len(s.tabs()) == 2)
        check("the verb still makes tabs", len(s.tabs()) == 2, str(len(s.tabs())))

    top = _cfg("newtab_button false\n")
    with Session(SH, cols=90, rows=20, config=top) as s:
        s.settle(30)
        snap = s.snapshot()
        check(
            "and the top strip loses it too",
            all(h["action"] != "newtab" for h in snap.hits),
            str([h for h in snap.hits if h["action"] == "newtab"]),
        )


def test_a_narrow_sidebar_keeps_the_bare_mark():
    narrow = _cfg('tab_bar_side "left"\ntab_bar_width 10\n')
    with Session(SH, cols=90, rows=20, config=narrow) as s:
        s.settle(40)
        snap = s.snapshot()
        row = snap.line(Y0 + 1 + NT_PAD)[CX : CX + 8]
        check(
            "no room for the word, so it is not said", "new tab" not in row, repr(row)
        )
        check(
            "...but the button is still there",
            snap.hit_at(CX, Y0 + 1 + NT_PAD) == "newtab",
            str(snap.hit_at(CX, Y0 + 1 + NT_PAD)),
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
    test_the_index_sits_at_the_far_end()
    test_the_prefix_is_still_there_for_anyone_who_wants_it()
    test_the_sidebar_aligns_by_default()
    test_a_long_name_stops_short_of_the_number()
    test_a_narrow_row_keeps_the_prefix()
    test_the_top_strip_ignores_it()
    test_the_whole_row_is_still_one_door()
    test_a_bad_index_mode_is_a_line_and_no_more()
    test_every_tab_sits_on_a_plate()
    test_an_old_theme_gets_an_idle_plate_in_its_own_family()
    test_a_theme_can_have_the_bare_tabs_back()
    test_status_rows_sit_under_their_tab()
    test_status_rows_are_told_apart_by_the_slant_not_by_an_indent()
    test_status_rows_take_their_own_theme_colours()
    test_an_old_theme_gets_coherent_status_colours_anyway()
    test_a_monochrome_theme_stays_monochrome()
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
    test_newtab_pad_is_the_air_before_the_button()
    test_newtab_button_false_removes_it_everywhere()
    test_tab_gap_is_air_between_tabs_and_nothing_else()
    test_tab_gap_gives_way_before_a_tab_does()
    test_statuses_never_cost_a_tab_its_row()
    test_a_status_can_be_given_more_than_one_row()
    test_one_line_still_cuts_at_the_column_not_the_word()
    test_wrapped_statuses_are_striped_so_panes_stay_apart()
    test_every_row_of_a_status_is_the_same_door()
    test_wrapping_still_says_when_a_status_did_not_fit()
    test_a_narrow_sidebar_keeps_the_bare_mark()
    test_the_strip_keeps_its_bare_mark()
    test_narrow_terminal_falls_back_to_top()
    test_growing_back_restores_the_sidebar()
    sys.exit(report())
