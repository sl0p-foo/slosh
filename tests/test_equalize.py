#!/usr/bin/env python3
"""`C-a =`: every visible pane gets an even share of the rows and columns.

The interesting part is not the flat case -- three columns nudged apart and put
back is arithmetic -- but what "even" means once the tree has depth. A split is
divided between the *panes behind* each child, so one pane beside a stack of
two is a third of the width and not half of it. Weighting per child would make
"even" mean something different at every depth, which is the layout this action
exists to get away from.
"""

import sys

from harness import Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]


def by_pos(s):
    """Panes on screen, left to right then top to bottom. A minimised pane gets
    no rect at all — the strip draws it — so it is not one of these."""
    return sorted((p for p in s.panes() if p["w"] > 1), key=lambda p: (p["x"], p["y"]))


def widths(s):
    return [p["w"] for p in by_pos(s)]


def test_a_nudged_row_of_columns_goes_back_to_even():
    with Session(SH, cols=120, rows=24) as s:
        s.settle()
        s.key("\\\\")
        s.key("\\\\")
        s.settle()
        even = widths(s)
        check("three columns start out even", max(even) - min(even) <= 1, str(even))

        for _ in range(4):
            s.key("L")  # shift+l: move the boundary right
        s.settle()
        nudged = widths(s)
        check(
            "nudging the boundary makes them uneven",
            max(nudged) - min(nudged) > 2,
            str(nudged),
        )

        s.key("0")
        s.settle()
        back = widths(s)
        check("equalize puts them back to even", max(back) - min(back) <= 1, str(back))
        check("and it kept every pane", len(back) == 3, str(back))


def test_it_divides_a_split_between_the_panes_behind_it():
    """A pane beside a stack of two: a third of the width, not half of it."""
    with Session(SH, cols=120, rows=30) as s:
        s.settle()
        s.key("\\\\")  # left | right
        s.key("-")  # right becomes right-top / right-bottom
        s.settle()
        left, top, bottom = by_pos(s)
        check(
            "the split starts out halved per child",
            abs(left["w"] - top["w"]) <= 1,
            f"{left['w']} vs {top['w']}",
        )

        s.key("0")
        s.settle()
        left, top, bottom = by_pos(s)
        check(
            "the stack of two gets twice the lone pane's width",
            abs(top["w"] - 2 * left["w"]) <= 2,
            f"{left['w']} vs {top['w']}",
        )
        check(
            "the two panes sharing that width still match",
            top["w"] == bottom["w"],
            f"{top['w']} vs {bottom['w']}",
        )
        check(
            "and they still split their column in half",
            abs(top["h"] - bottom["h"]) <= 1,
            f"{top['h']} vs {bottom['h']}",
        )
        check(
            "the lone pane still spans the tab",
            left["h"] > top["h"],
            f"{left['h']} vs {top['h']}",
        )


def test_a_minimised_pane_is_not_counted():
    """It is out of the layout and costs a row in the strip, not a share: its
    share would otherwise be handed to a pane nobody can see."""
    with Session(SH, cols=120, rows=30) as s:
        s.settle()
        s.key("\\\\")
        s.key("-")
        s.settle()
        top = by_pos(s)[1]
        s.api("focus", id=top["id"])
        s.key("m")
        s.settle()
        check("one pane is away in the strip", len(widths(s)) == 2, str(widths(s)))

        s.key("0")
        s.settle()
        seen = widths(s)
        check(
            "the two on screen share the tab evenly",
            max(seen) - min(seen) <= 1,
            str(seen),
        )


def test_one_pane_has_nothing_to_even_out():
    with Session(SH, cols=80, rows=24) as s:
        s.settle()
        before = s.pane()
        s.key("0")
        s.settle()
        snap = s.snapshot()
        check(
            "it says so rather than doing nothing quietly",
            "nothing to even out" in snap.screen(),
            repr(snap.screen()[-200:]),
        )
        after = s.pane()
        check(
            "and the pane is untouched",
            (after["x"], after["y"], after["w"], after["h"])
            == (before["x"], before["y"], before["w"], before["h"]),
            str((before, after)),
        )


def test_it_is_in_the_palette_and_the_cheatsheet():
    # Room for the whole sheet: it is sized from the screen, and a sheet
    # clipped at the bottom would make this a test of the box.
    with Session(SH, cols=110, rows=40) as s:
        s.settle()
        s.key("?")
        s.settle()
        sheet = s.snapshot().screen()
        check(
            "the cheatsheet lists it under size, with its key",
            "0      even out every split" in sheet,
            repr(sheet[:400]),
        )
        s.send("q")  # any key closes the sheet

        s.key("p")
        s.send("equalize")
        s.settle()
        pal = s.snapshot()
        check(
            "the palette finds it by its config name",
            "even out every split" in pal.screen(),
            repr(pal.screen()[:400]),
        )
        check(
            "with the key it would have skipped",
            "C-a 0" in pal.screen(),
            repr(pal.screen()[:400]),
        )
        check(
            "and the row runs it",
            any(h["action"].startswith("run:") for h in pal.hits),
            str([h["action"] for h in pal.hits][:8]),
        )


if __name__ == "__main__":
    test_a_nudged_row_of_columns_goes_back_to_even()
    test_it_divides_a_split_between_the_panes_behind_it()
    test_a_minimised_pane_is_not_counted()
    test_one_pane_has_nothing_to_even_out()
    test_it_is_in_the_palette_and_the_cheatsheet()
    sys.exit(report())
