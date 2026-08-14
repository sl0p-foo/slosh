#!/usr/bin/env python3
"""Resizing two boundaries at once, where a row boundary crosses a column one.

The two never overlap in the tree -- a column boundary stops where the row
boundary begins, because they belong to different splits -- so the corner is
found by adjacency, and it has to move every boundary that meets there or the
line it looks like would come apart.
"""
import os
import sys
import tempfile
import time

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']
CROSS, CROSS_ON = "\u253c", "\u256c"

GRID = ('layout {\n tab name="t" {\n  pane split="rows" {\n'
        '   pane split="cols" { pane\n    pane }\n'
        '   pane split="cols" { pane\n    pane }\n  }\n }\n}\n')
COLUMNS = 'layout {\n tab name="t" {\n  pane\n  pane\n }\n}\n'


def lay(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def corners(s):
    return [h for h in s.snapshot().hits if h["action"].startswith("corner:")]


def rects(s):
    return {p["id"]: (p["x"], p["y"], p["w"], p["h"]) for p in s.panes()}


def test_a_grid_has_a_corner_where_the_boundaries_cross():
    l = lay(GRID)
    with Session(SH, cols=60, rows=20, layout=l) as s:
        s.settle(30)
        c = corners(s)
        check("the crossing is a target of its own", len(c) == 1, str(c))
        if not c:
            return
        gaps = [h for h in s.snapshot().hits if h["action"].startswith("edge:")]
        h_gap = [g for g in gaps if g["h"] == 1][0]
        v_gap = [g for g in gaps if g["h"] > 1][0]
        check("it sits on the row boundary", c[0]["y"] == h_gap["y"], str(c[0]))
        check("and in the column boundary's columns",
              c[0]["x"] == v_gap["x"] and c[0]["w"] == v_gap["w"], str(c[0]))
        # Registered after both gaps, so those cells resolve to the corner.
        snap = s.snapshot()
        check("and wins those cells from both of them",
              snap.hit_at(c[0]["x"], c[0]["y"]) == c[0]["action"],
              str(snap.hit_at(c[0]["x"], c[0]["y"])))
    os.unlink(l)


def test_a_layout_with_no_crossing_has_no_corner():
    l = lay(COLUMNS)
    with Session(SH, cols=60, rows=20, layout=l) as s:
        s.settle(30)
        check("two columns cross nothing", corners(s) == [], str(corners(s)))
    os.unlink(l)


def test_dragging_it_moves_both_boundaries():
    l = lay(GRID)
    with Session(SH, cols=60, rows=20, layout=l) as s:
        s.settle(30)
        c = corners(s)[0]
        before = rects(s)
        ids = sorted(before)
        top_left, top_right, bot_left = ids[0], ids[1], ids[2]

        s.send(rf"\e[<0;{c['x'] + 1};{c['y'] + 1}M")
        s.send(rf"\e[<32;{c['x'] + 6};{c['y'] + 4}M")
        s.send(rf"\e[<0;{c['x'] + 6};{c['y'] + 4}m")
        s.settle(40)
        after = rects(s)

        check("the column boundary moved right",
              after[top_left][2] > before[top_left][2]
              and after[top_right][2] < before[top_right][2],
              f"{before[top_left]} -> {after[top_left]}")
        check("and the row boundary moved down",
              after[top_left][3] > before[top_left][3],
              f"{before[top_left]} -> {after[top_left]}")
        # The two column boundaries are separate splits that happen to line up.
        # Moving one without the other would put a step in one apparent line.
        check("both column boundaries moved together, so the line is still one",
              after[bot_left][2] == after[top_left][2],
              f"top {after[top_left]} bottom {after[bot_left]}")
    os.unlink(l)


def test_resting_on_it_arms_both_boundaries():
    l = lay(GRID)
    with Session(SH, cols=60, rows=20, layout=l) as s:
        s.settle(30)
        c = corners(s)[0]
        screen = s.snapshot().screen()
        check("nothing armed before the pointer rests",
              CROSS not in screen and "\u250a" not in screen, "")

        s.send(rf"\e[<35;{c['x'] + 1};{c['y'] + 1}M")
        time.sleep(0.35)
        s.settle(60)
        snap = s.snapshot()
        check("the crossing is marked", CROSS in snap.screen(),
              repr(snap.line(c["y"])))
        check("the column boundary is armed too", "\u250a" in snap.screen(), "")
        check("and the row boundary", "\u2508" in snap.screen(), "")

        s.send(rf"\e[<0;{c['x'] + 1};{c['y'] + 1}M")
        s.send(rf"\e[<32;{c['x'] + 3};{c['y'] + 2}M")
        s.settle(40)
        check("and it reads differently once you have hold of it",
              CROSS_ON in s.snapshot().screen(), repr(s.snapshot().screen()[:200]))
        s.send(rf"\e[<0;{c['x'] + 3};{c['y'] + 2}m")
        s.settle(20)
    os.unlink(l)


def test_the_hint_says_both_ways():
    l = lay(GRID)
    with Session(SH, cols=90, rows=20, layout=l) as s:
        s.settle(30)
        c = corners(s)[0]
        s.send(rf"\e[<35;{c['x'] + 1};{c['y'] + 1}M")
        s.settle(30)
        check("the status line says what it would do",
              "both ways" in s.snapshot().text[-2], repr(s.snapshot().text[-2]))
    os.unlink(l)


if __name__ == "__main__":
    test_a_grid_has_a_corner_where_the_boundaries_cross()
    test_a_layout_with_no_crossing_has_no_corner()
    test_dragging_it_moves_both_boundaries()
    test_resting_on_it_arms_both_boundaries()
    test_the_hint_says_both_ways()
    sys.exit(report())
