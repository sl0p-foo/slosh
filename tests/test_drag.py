#!/usr/bin/env python3
"""M8: pane resizing and drag-to-reorder — one drag machine, two verbs.

Sizes are weights, so resizing is not a special case of anything: an even
split is equal weights, and every layout pass is still the same pure function
of the tree and the rect.
"""
import sys
import time

from harness import Session, check, report

def _cfg(text):
    import tempfile
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


# The hint is gated on a real clock, so the wait is real - just a short one.
FAST = _cfg("hover_delay_ms 20\n")
def _cfg(text):
    import tempfile
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


# The resize hint arms on a real clock, so its tests really do wait - but only
# the ones *about* the dwell need the default 250ms to prove anything.
FAST = _cfg("hover_delay_ms 20\n")
SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']


def widths(s):
    return [p["w"] for p in s.panes()]


def test_keyboard_resize():
    with Session(SH, cols=80, rows=16, config=FAST) as s:
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
    with Session(SH, cols=80, rows=20, config=FAST) as s:
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
    with Session(SH, cols=100, rows=20, config=FAST) as s:
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
    with Session(SH, cols=150, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        before = [(p["id"], p["x"]) for p in s.panes()]
        left = s.panes()[0]

        # a drag that wanders and comes back must change nothing. (A press and
        # release with no motion is not this: that is a click, and a click on
        # a border splits — see test_split_ux.py.)
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}M")
        s.send(rf"\e[<32;{left['x'] + 20};{left['y'] + 4}M")
        s.send(rf"\e[<32;{left['x'] + 5};{left['y'] + 1}M")
        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}m")
        s.settle()
        check("a drag that returns to its origin changes nothing",
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

        # and a border click still works, which is the proof it is not wedged
        s.click(left["x"], left["y"] + 2)
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


# ---- the resize zone announces itself --------------------------------------

GRIP_V, GRAB_V = "\u250a", "\u2551"   # dotted / doubled, vertical boundary
GRIP_H, GRAB_H = "\u2508", "\u2550"   # dotted / doubled, horizontal boundary
ARROW_V, ARROW_H = "\u21d4", "\u21d5"  # which way that boundary travels


def test_the_gap_says_it_is_a_handle():
    # No FAST here: this is the test that proves contact alone is not enough,
    # so it needs a dwell long enough to be observably not-yet-elapsed.
    with Session(SH, cols=120, rows=26) as s:
        s.settle()
        s.key("\\\\")
        s.settle(200)
        left, right = s.panes()
        gx, gy = left["x"] + left["w"], left["y"] + 3

        check("an idle gap is blank", GRIP_V not in s.snapshot().screen(), "")

        # Crossing a gap on the way somewhere is the most ordinary mouse
        # movement there is, so contact alone must not arm it.
        s.send(rf"\e[<35;{gx + 1};{gy + 1}M")
        s.settle(60)
        check("contact alone does not arm it",
              GRIP_V not in s.snapshot().screen(), "")

        time.sleep(0.35)
        s.settle(60)
        snap = s.snapshot()
        check("resting on it says it can be moved", GRIP_V in snap.screen(), "")
        check("and does not yet claim it is being moved",
              GRAB_V not in snap.screen(), "")

        pos = snap.find(GRIP_V)
        check("the hint is in the gap, not in either pane",
              pos and left["x"] + left["w"] <= pos[0] < right["x"], str(pos))


def test_the_hint_changes_while_resizing():
    with Session(SH, cols=120, rows=26) as s:
        s.settle()
        s.key("\\\\")
        s.settle(200)
        left, right = s.panes()
        gx, gy = left["x"] + left["w"], left["y"] + 3
        before = left["w"]

        s.send(rf"\e[<0;{gx + 1};{gy + 1}M")
        s.send(rf"\e[<32;{gx + 6};{gy + 1}M")
        s.settle(80)
        snap = s.snapshot()
        check("a resize in progress reads differently from a hover",
              GRAB_V in snap.screen() and GRIP_V not in snap.screen(), "")

        s.send(rf"\e[<0;{gx + 6};{gy + 1}m")
        s.settle(60)
        check("and the boundary actually moved",
              s.panes()[0]["w"] > before, f'{before} -> {s.panes()[0]["w"]}')
        # Off the handle: still resting on it would (correctly) re-arm the
        # dotted hint, which is the pointer following the mouse, not a leak.
        s.send(rf"\e[<35;{left["x"] + 3};{gy + 1}M")
        snap = s.snapshot()
        check("both hints are gone once it is dropped and left alone",
              GRAB_V not in snap.screen() and GRIP_V not in snap.screen(), "")


def test_a_horizontal_boundary_uses_horizontal_marks():
    with Session(SH, cols=120, rows=26, config=FAST) as s:
        s.settle()
        s.key("-")          # split into rows
        s.settle(200)
        top, bottom = s.panes()
        gx, gy = top["x"] + 6, top["y"] + top["h"]

        s.send(rf"\e[<35;{gx + 1};{gy + 1}M")
        time.sleep(0.06)
        s.settle(60)
        snap = s.snapshot()
        check("a row boundary hints along its own axis",
              GRIP_H in snap.screen() and GRIP_V not in snap.screen(), "")
        pos = snap.find(GRIP_H)
        check("in the gap between the two rows",
              pos and top["y"] + top["h"] <= pos[1] < bottom["y"], str(pos))


def test_no_resize_hint_during_another_drag():
    """A pointer already carrying a pane is not shopping for a boundary."""
    with Session(SH, cols=120, rows=26, config=FAST) as s:
        s.settle()
        s.key("\\\\")
        s.settle(200)
        left, right = s.panes()
        gx, gy = left["x"] + left["w"], left["y"] + 3

        s.send(rf"\e[<0;{left['x'] + 5};{left['y'] + 1}M")   # grab the title
        s.send(rf"\e[<32;{gx + 1};{gy + 1}M")                # drag over the gap
        time.sleep(0.06)
        s.settle(80)
        check("a title drag over a gap arms nothing",
              GRIP_V not in s.snapshot().screen(), "")
        s.send(rf"\e[<0;{gx + 1};{gy + 1}m")
        s.settle(80)


def test_the_gap_names_the_verb():
    """The dots say it is a handle; the arrow says what pulling it does."""
    with Session(SH, cols=120, rows=26, config=FAST) as s:
        s.settle()
        s.key("\\\\")
        s.settle(200)
        left, right = s.panes()
        gx, gy = left["x"] + left["w"], left["y"] + 3

        s.send(rf"\e[<35;{gx + 1};{gy + 1}M")
        time.sleep(0.06)
        s.settle(60)
        snap = s.snapshot()
        check("hovering a column boundary shows a sideways arrow",
              ARROW_V in snap.screen(), "")
        # A divider between columns is drawn vertically and travels sideways;
        # the useful half of that is the travelling.
        check("and not the up-down one", ARROW_H not in snap.screen(), "")

        pos = snap.find(ARROW_V)
        check("the arrow sits in the middle of the boundary",
              pos and pos[1] == left["y"] + left["h"] // 2, str(pos))
        check("on the line itself, inside the gap",
              pos and left["x"] + left["w"] <= pos[0] < right["x"], str(pos))

        s.send(rf"\e[<0;{gx + 1};{gy + 1}M")
        s.send(rf"\e[<32;{gx + 5};{gy + 1}M")
        s.settle(80)
        check("it stays while the boundary is actually moving",
              ARROW_V in s.snapshot().screen(), "")
        s.send(rf"\e[<0;{gx + 5};{gy + 1}m")
        s.send(rf"\e[<35;{left["x"] + 3};{gy + 1}M")   # and off the handle
        check("and goes with the rest of the hint",
              ARROW_V not in s.snapshot().screen(), "")


def test_a_row_boundary_names_its_own_verb():
    with Session(SH, cols=120, rows=26, config=FAST) as s:
        s.settle()
        s.key("-")
        s.settle(200)
        top, bottom = s.panes()
        gx, gy = top["x"] + 6, top["y"] + top["h"]

        s.send(rf"\e[<35;{gx + 1};{gy + 1}M")
        time.sleep(0.06)
        s.settle(60)
        snap = s.snapshot()
        check("a row boundary shows an up-down arrow",
              ARROW_H in snap.screen() and ARROW_V not in snap.screen(), "")
        pos = snap.find(ARROW_H)
        check("in the gap between the rows",
              pos and top["y"] + top["h"] <= pos[1] < bottom["y"], str(pos))


if __name__ == "__main__":
    test_keyboard_resize()
    test_vertical_resize()
    test_resize_survives_a_collapse_cycle()
    test_edge_drag()
    test_title_drag_reorders()
    test_drag_edge_cases()
    test_drop_target_is_visible()
    test_the_gap_says_it_is_a_handle()
    test_the_hint_changes_while_resizing()
    test_a_horizontal_boundary_uses_horizontal_marks()
    test_no_resize_hint_during_another_drag()
    test_the_gap_names_the_verb()
    test_a_row_boundary_names_its_own_verb()
    sys.exit(report())
