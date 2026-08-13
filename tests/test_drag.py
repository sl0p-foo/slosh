#!/usr/bin/env python3
"""M8: pane resizing and drag-to-reorder — one drag machine, two verbs.

Sizes are weights, so resizing is not a special case of anything: an even
split is equal weights, and every layout pass is still the same pure function
of the tree and the rect.
"""
import sys

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']


def widths(s):
    return [p["w"] for p in s.panes()]


def test_keyboard_resize():
    with Session(SH, cols=80, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        before = widths(s)
        check("an even split starts even", abs(before[0] - before[1]) <= 1,
              str(before))

        s.send(r"\x01L")  # C-a shift-L: move the boundary right
        s.settle()
        after = widths(s)
        check("resize moves the boundary right",
              after[0] > before[0] and after[1] < before[1], f"{before} -> {after}")

        s.send(r"\x01H")
        s.settle()
        check("and back left again", widths(s) == before, f"{before} -> {widths(s)}")

        for _ in range(20):
            s.send(r"\x01L")
        s.settle()
        squeezed = widths(s)
        check("a pane can be squeezed but not squeezed out", squeezed[1] >= 3,
              str(squeezed))
        check("the total is still the screen",
              sum(squeezed) + 2 + 4 == 80, str(squeezed))


def test_vertical_resize():
    with Session(SH, cols=80, rows=20) as s:
        s.settle()
        s.key("-")
        s.settle()
        before = [p["h"] for p in s.panes()]
        s.send(r"\x01J")
        s.settle()
        after = [p["h"] for p in s.panes()]
        check("resize works in rows too", after[0] > before[0],
              f"{before} -> {after}")

        s.send(r"\x01L")  # no column split anywhere above this pane
        s.settle()
        check("resizing against a boundary that does not exist is harmless",
              [p["h"] for p in s.panes()] == after and s.api("alive")["alive"],
              str(s.panes()))


def test_resize_survives_a_collapse_cycle():
    with Session(SH, cols=100, rows=20) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        s.send(r"\x01L")
        s.send(r"\x01L")
        s.settle()
        resized = widths(s)

        s.resize(40, 12)   # collapses
        s.settle()
        s.resize(100, 20)  # and back
        s.settle()
        check("weights survive a collapse/expand cycle", widths(s) == resized,
              f"{resized} -> {widths(s)}")


def test_edge_drag():
    with Session(SH, cols=80, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        panes = s.panes()
        snap = s.snapshot()

        gap_x = panes[0]["x"] + panes[0]["w"]
        gap_y = panes[0]["y"] + 2
        action = snap.hit_at(gap_x, gap_y)
        check("the gap between panes is a drag target",
              action and action.startswith("edge:"), str(action))

        before = widths(s)
        # press in the gap, move right, release
        s.send(rf"\e[<0;{gap_x + 1};{gap_y + 1}M")
        s.send(rf"\e[<32;{gap_x + 9};{gap_y + 1}M")
        s.send(rf"\e[<0;{gap_x + 9};{gap_y + 1}m")
        s.settle()
        after = widths(s)
        check("dragging the gap right widens the left pane",
              after[0] > before[0] and after[1] < before[1],
              f"{before} -> {after}")

        # and back
        s.send(rf"\e[<0;{after[0] + 3};{gap_y + 1}M")
        s.send(rf"\e[<32;{after[0] - 5};{gap_y + 1}M")
        s.send(rf"\e[<0;{after[0] - 5};{gap_y + 1}m")
        s.settle()
        check("dragging it back narrows it again", widths(s)[0] < after[0],
              f"{after} -> {widths(s)}")


def test_title_drag_reorders():
    with Session(SH, cols=90, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        # write into each so we can see them move
        s.api("focus", id=left["id"])
        s.raw("LEFTMARK")
        s.api("focus", id=right["id"])
        s.raw("RIGHTMARK")
        s.settle()

        snap = s.snapshot()
        check("a pane title row is a drag handle",
              snap.hit_at(left["x"] + 4, left["y"]) == f"title:{left['id']}",
              str(snap.hit_at(left["x"] + 4, left["y"])))

        # drag the left pane's title onto the right pane
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}M")
        s.send(rf"\e[<32;{right['x'] + 5};{right['y'] + 3}M")
        s.settle(60)
        s.send(rf"\e[<0;{right['x'] + 5};{right['y'] + 3}m")
        s.settle()

        panes = {p["id"]: p for p in s.panes()}
        check("the dragged pane took the target's place",
              panes[left["id"]]["x"] > panes[right["id"]]["x"],
              f"{panes[left['id']]['x']} vs {panes[right['id']]['x']}")

        snap = s.snapshot()
        check("and its content came with it",
              "LEFTMARK" in snap.pane_text(panes[left["id"]]),
              repr(snap.pane_text(panes[left["id"]])))
        check("the other pane's content did too",
              "RIGHTMARK" in snap.pane_text(panes[right["id"]]),
              repr(snap.pane_text(panes[right["id"]])))


def test_drag_edge_cases():
    with Session(SH, cols=90, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        before = [(p["id"], p["x"]) for p in s.panes()]
        left = s.panes()[0]

        # a drag that ends where it started must change nothing
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}M")
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}m")
        s.settle()
        check("dropping a pane on itself does nothing",
              [(p["id"], p["x"]) for p in s.panes()] == before, str(s.panes()))

        # a drag released over nothing (the gap ring) must not swap or crash
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}M")
        s.send(r"\e[<32;1;1M")
        s.send(r"\e[<0;1;1m")
        s.settle()
        check("dropping outside any pane is harmless",
              [(p["id"], p["x"]) for p in s.panes()] == before
              and s.api("alive")["alive"], str(s.panes()))

        # a press whose release never arrives must not wedge the mouse
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}M")
        s.send("k")  # any keystroke ends a drag
        s.settle(60)
        check("a keystroke ends an abandoned drag", s.api("alive")["alive"])
        snap = s.snapshot()
        pos = snap.line(left["y"]).find("+")
        if pos >= 0:
            s.click(pos, left["y"])
            s.settle()
            check("the mouse still works after a drag", len(s.panes()) == 3,
                  str(len(s.panes())))


def test_drop_target_is_visible():
    with Session(SH, cols=90, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}M")
        s.send(rf"\e[<32;{right['x'] + 5};{right['y'] + 3}M")
        s.settle(60)
        snap = s.snapshot()
        run = snap.style_at(right["x"], right["y"])
        check("the drop target is highlighted while dragging",
              run and run["fg"] == "#ff5fd7" and "bold" in run["attrs"], str(run))
        s.send(rf"\e[<0;{right['x'] + 5};{right['y'] + 3}m")


if __name__ == "__main__":
    test_keyboard_resize()
    test_vertical_resize()
    test_resize_survives_a_collapse_cycle()
    test_edge_drag()
    test_title_drag_reorders()
    test_drag_edge_cases()
    test_drop_target_is_visible()
    sys.exit(report())
