#!/usr/bin/env python3
"""Splitting by clicking a border, with a guide that shows what will happen.

The old per-pane `+` was one glyph for a verb that has four directions, and it
taxed every frame three columns forever. A border encodes the direction by
being on that side, costs nothing when idle, and shows a preview on hover.
"""
import sys

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;shell\\007"; stty raw -echo; cat']


def hover(s, x, y):
    s.send(rf"\e[<35;{x + 1};{y + 1}M")


def click(s, x, y):
    s.send(rf"\e[<0;{x + 1};{y + 1}M")
    s.send(rf"\e[<0;{x + 1};{y + 1}m")


def test_no_more_plus():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        snap = s.snapshot()
        p = s.pane()
        check("the frame has no split button any more",
              "+" not in snap.line(p["y"]), repr(snap.line(p["y"])))
        check("the status bar still has its own, different one",
              "+tab" in snap.line(1), repr(snap.line(1)))
        check("the title gets the columns back",
              snap.line(p["y"]).count("─") > p["w"] - 12, repr(snap.line(p["y"])))


def test_each_border_splits_toward_itself():
    for side, cell, expect in [
        ("right", lambda p: (p["x"] + p["w"] - 1, p["y"] + 2), "right"),
        ("left", lambda p: (p["x"], p["y"] + 2), "left"),
        ("bottom", lambda p: (p["x"] + 4, p["y"] + p["h"] - 1), "below"),
        ("top", lambda p: (p["x"] + 4, p["y"]), "above"),
    ]:
        with Session(SH, cols=70, rows=18) as s:
            s.settle()
            before = s.pane()
            x, y = cell(before)
            click(s, x, y)
            s.settle(80)
            panes = s.panes()
            check(f"clicking the {side} border splits", len(panes) == 2,
                  str(len(panes)))
            if len(panes) != 2:
                continue

            old = [q for q in panes if q["id"] == before["id"]][0]
            new = [q for q in panes if q["id"] != before["id"]][0]
            if expect == "right":
                ok = new["x"] > old["x"] and new["y"] == old["y"]
            elif expect == "left":
                ok = new["x"] < old["x"] and new["y"] == old["y"]
            elif expect == "below":
                ok = new["y"] > old["y"] and new["x"] == old["x"]
            else:
                ok = new["y"] < old["y"] and new["x"] == old["x"]
            check(f"the new pane lands {expect}", ok,
                  f"old={old['x']},{old['y']} new={new['x']},{new['y']}")
            check("and takes focus", new["focused"], str(new))
            check("and it says which way it went",
                  f"split {side if side != 'bottom' else 'down'}"
                  .replace("split top", "split up") in s.snapshot().screen()
                  or "split" in s.snapshot().screen(),
                  repr(s.snapshot().screen()[-80:]))


def test_the_guide():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        snap = s.snapshot()
        check("no guide when the pointer is elsewhere",
              "┃" not in snap.screen() and "╎" not in snap.screen(),
              repr(snap.screen()[:120]))

        hover(s, p["x"], p["y"] + 2)  # left border
        s.settle(60)
        snap = s.snapshot()
        check("hovering a side border arms it", "┃" in snap.screen(),
              repr(snap.screen()[:200]))
        check("and previews where the split would land", "╎" in snap.screen(),
              repr(snap.screen()[:200]))

        hover(s, p["x"] + 4, p["y"] + p["h"] - 1)  # bottom border
        s.settle(60)
        snap = s.snapshot()
        check("hovering the bottom border arms that instead",
              "━" in snap.screen() and "╌" in snap.screen(), repr(snap.screen()[:200]))
        check("and the side guide is gone", "┃" not in snap.screen(),
              repr(snap.screen()[:200]))

        hover(s, p["content_x"] + 3, p["content_y"] + 2)  # into the content
        s.settle(60)
        snap = s.snapshot()
        check("moving off a border puts the guide away",
              "━" not in snap.screen() and "┃" not in snap.screen(),
              repr(snap.screen()[:200]))


def test_guide_is_per_pane():
    with Session(SH, cols=90, rows=14) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        hover(s, left["x"], left["y"] + 2)
        s.settle(60)
        snap = s.snapshot()
        row = snap.line(left["y"] + 2)
        check("the hovered pane is armed", "┃" in row[left["x"]:left["x"] + left["w"]],
              repr(row))
        check("the other one is not",
              "┃" not in row[right["x"]:right["x"] + right["w"]], repr(row))


def test_drag_still_moves():
    """The top border does double duty: click splits, drag moves."""
    with Session(SH, cols=90, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()

        # drag: press, move, release -> swap, not split
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}M")
        s.send(rf"\e[<32;{right['x'] + 5};{right['y'] + 3}M")
        s.send(rf"\e[<0;{right['x'] + 5};{right['y'] + 3}m")
        s.settle(80)
        check("dragging the top border still moves the pane",
              len(s.panes()) == 2, str(len(s.panes())))
        panes = {q["id"]: q for q in s.panes()}
        check("and swaps them", panes[left["id"]]["x"] > panes[right["id"]]["x"],
              str(s.panes()))

        # click: press, release, no motion -> split up
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}M")
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}m")
        s.settle(80)
        check("clicking it splits instead", len(s.panes()) == 3,
              str(len(s.panes())))


def test_tiny_panes_have_no_targets():
    with Session(SH, cols=90, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        s.resize(30, 8)  # collapses to a stack
        s.settle(80)
        snap = s.snapshot()
        check("a collapsed layout still draws and does not crash",
              s.api("alive")["alive"], "")
        check("and its header is a focus target, not a split one",
              all(not (h["action"].startswith("border:") and h["h"] == 1
                       and h["w"] < 4) for h in snap.hits), str(snap.hits))


if __name__ == "__main__":
    test_no_more_plus()
    test_each_border_splits_toward_itself()
    test_the_guide()
    test_guide_is_per_pane()
    test_drag_still_moves()
    test_tiny_panes_have_no_targets()
    sys.exit(report())
