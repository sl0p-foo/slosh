#!/usr/bin/env python3
"""Splitting by clicking a border, with a guide that shows what will happen.

The old per-pane `+` was one glyph for a verb that has four directions, and it
taxed every frame three columns forever. A border encodes the direction by
being on that side, costs nothing when idle, and shows a preview on hover.
"""
import os
import sys
import tempfile
import time

from harness import Session, check, report


FAST_TEXT = "hover_delay_ms 20\n"


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


FAST = cfg(FAST_TEXT)

SH = ["/bin/sh", "-c", 'printf "\\033]2;shell\\007"; stty raw -echo; cat']


def hover(s, x, y):
    s.send(rf"\e[<35;{x + 1};{y + 1}M")


def rest(s, x, y):
    """Hover and stay there: the guide arms on dwell, not on contact.

    The wait is real elapsed time - the guide is gated on a clock, not on
    anything observable - so the sessions that are not *about* the dwell run
    with FAST_TEXT and pay 20ms instead of 250."""
    hover(s, x, y)
    time.sleep(0.06)


def click(s, x, y):
    s.send(rf"\e[<0;{x + 1};{y + 1}M")
    s.send(rf"\e[<0;{x + 1};{y + 1}m")


def test_no_more_plus():
    with Session(SH, cols=60, rows=14, config=FAST) as s:
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
        with Session(SH, cols=80, rows=26, config=FAST) as s:
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
    with Session(SH, cols=80, rows=26, config=FAST) as s:
        s.settle()
        p = s.pane()
        snap = s.snapshot()
        check("no guide when the pointer is elsewhere",
              "┃" not in snap.screen() and "╎" not in snap.screen(),
              repr(snap.screen()[:120]))

        rest(s, p["x"], p["y"] + 2)  # left border
        snap = s.snapshot()
        check("hovering a side border arms it", "┃" in snap.screen(),
              repr(snap.screen()[:200]))
        check("and previews where the split would land", "╎" in snap.screen(),
              repr(snap.screen()[:200]))

        rest(s, p["x"] + 4, p["y"] + p["h"] - 1)  # bottom border
        snap = s.snapshot()
        check("hovering the bottom border arms that instead",
              "━" in snap.screen() and "╌" in snap.screen(), repr(snap.screen()[:200]))
        check("and the side guide is gone", "┃" not in snap.screen(),
              repr(snap.screen()[:200]))

        rest(s, p["content_x"] + 3, p["content_y"] + 2)  # into the content
        snap = s.snapshot()
        check("moving off a border puts the guide away",
              "━" not in snap.screen() and "┃" not in snap.screen(),
              repr(snap.screen()[:200]))


def test_guide_follows_the_new_layout():
    """Regression: the guide described the layout from *before* the split.

    Hover state used to be remembered from the last motion event, so after a
    click split the guide stayed attached to the old pane and the old edge
    until the mouse moved and corrected it. It is derived during the paint
    now, from the rects that paint registered.
    """
    with Session(SH, cols=120, rows=26, config=FAST) as s:
        s.settle()
        p = s.pane()
        edge_x, edge_y = p["x"] + p["w"] - 1, p["y"] + 3

        rest(s, edge_x, edge_y)
        click(s, edge_x, edge_y)
        s.settle(100)  # and deliberately no further mouse movement

        snap = s.snapshot()
        panes = s.panes()
        check("the split happened", len(panes) == 2, str(len(panes)))

        armed = [q for q in panes
                 if "┃" in "".join(row[q["x"]:q["x"] + q["w"]] for row in snap.text)]
        check("exactly one pane shows a guide", len(armed) == 1, str(armed))
        if len(armed) != 1:
            return
        check("and it is the one the pointer is actually over",
              armed[0]["x"] <= edge_x < armed[0]["x"] + armed[0]["w"],
              f"pointer at {edge_x}, guide on {armed[0]['x']}..{armed[0]['x'] + armed[0]['w']}")
        check("on the edge the pointer is on",
              snap.hit_at(edge_x, edge_y) == f"border:{armed[0]['id']}:r",
              str(snap.hit_at(edge_x, edge_y)))


def test_the_guide_waits_for_a_rest():
    """A pointer crossing a border on its way elsewhere must not flash it."""
    with Session(SH, cols=80, rows=26, config=FAST) as s:
        s.settle()
        p = s.pane()
        left, right = p["x"], p["x"] + p["w"] - 1
        row = p["y"] + 3

        hover(s, left, row)
        snap = s.snapshot()  # no dwell: this is the moment of contact
        check("touching a border does not arm it immediately",
              "┃" not in snap.screen(), repr(snap.screen()[:160]))

        # keep moving: across the pane and onto the other border
        hover(s, left + 6, row)
        hover(s, right, row)
        snap = s.snapshot()
        check("passing over borders arms nothing", "┃" not in snap.screen(),
              repr(snap.screen()[:160]))

        time.sleep(0.4)  # now rest
        snap = s.snapshot()
        check("resting on one arms it", "┃" in snap.screen(),
              repr(snap.screen()[:200]))
        check("and it is the border being rested on",
              snap.hit_at(right, row) == f"border:{p['id']}:r",
              str(snap.hit_at(right, row)))

        hover(s, p["content_x"] + 2, row)
        snap = s.snapshot()
        check("moving away disarms it at once", "┃" not in snap.screen(),
              repr(snap.screen()[:160]))


def test_press_skips_the_wait():
    with Session(SH, cols=80, rows=26) as s:
        s.settle()
        p = s.pane()
        x, y = p["x"] + p["w"] - 1, p["y"] + 3
        s.send(rf"\e[<0;{x + 1};{y + 1}M")  # press and hold: that is intent
        snap = s.snapshot()
        check("holding the button on a border arms it immediately",
              "┃" in snap.screen(), repr(snap.screen()[:200]))
        s.send(rf"\e[<0;{x + 1};{y + 1}m")


def test_delay_is_configurable():
    import os, tempfile
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write("hover_delay_ms 0\n")
    f.close()
    with Session(SH, cols=80, rows=26, config=f.name) as s:
        s.settle()
        p = s.pane()
        hover(s, p["x"], p["y"] + 3)
        snap = s.snapshot()
        check("with no delay configured it arms on contact",
              "┃" in snap.screen(), repr(snap.screen()[:160]))
    os.unlink(f.name)


def test_guide_is_per_pane():
    with Session(SH, cols=150, rows=26, config=FAST) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        rest(s, left["x"], left["y"] + 2)
        snap = s.snapshot()
        row = snap.line(left["y"] + 2)
        check("the hovered pane is armed", "┃" in row[left["x"]:left["x"] + left["w"]],
              repr(row))
        check("the other one is not",
              "┃" not in row[right["x"]:right["x"] + right["w"]], repr(row))


def test_drag_still_moves():
    """The top border does double duty: click splits, drag moves."""
    with Session(SH, cols=90, rows=26, config=FAST) as s:
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
    with Session(SH, cols=90, rows=16, config=FAST) as s:
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


# ---- the floor: a split that is not worth making is not offered ------------

ARROWS = {"l": "\u25c4", "r": "\u25ba", "t": "\u25b2", "b": "\u25bc"}


def edge_of(pane, side):
    if side == "l":
        return pane["x"], pane["y"] + 2
    if side == "r":
        return pane["x"] + pane["w"] - 1, pane["y"] + 2
    if side == "t":
        return pane["x"] + 4, pane["y"]
    return pane["x"] + 4, pane["y"] + pane["h"] - 1


def test_a_split_below_the_floor_is_not_offered():
    """min_split is a usability floor, not the physical one: the pane below
    would still *fit* by min_pane, and is refused anyway."""
    conf = cfg(FAST_TEXT + "min_pane cols=10 rows=3\nmin_split cols=40 rows=14\n")
    with Session(SH, cols=80, rows=26, config=conf) as s:
        s.settle()
        p = s.pane()
        for side in ("l", "r", "t", "b"):
            x, y = edge_of(p, side)
            rest(s, x, y)
            snap = s.snapshot()
            body = snap.screen()
            check(f"resting on the {side} border arms nothing below the floor",
                  "\u2503" not in body and "\u2501" not in body, repr(body[:80]))
            click(s, x, y)
            s.settle(80)
            check(f"and clicking the {side} border splits nothing",
                  len(s.panes()) == 1, str(len(s.panes())))
    os.unlink(conf)


def test_the_floor_is_configurable_in_both_axes():
    # Wide enough for columns, too short for rows.
    conf = cfg(FAST_TEXT + "min_split cols=20 rows=40\n")
    with Session(SH, cols=90, rows=26, config=conf) as s:
        s.settle()
        p = s.pane()
        x, y = edge_of(p, "b")
        click(s, x, y)
        s.settle(80)
        check("a row split below the row floor is refused",
              len(s.panes()) == 1, str(len(s.panes())))
        x, y = edge_of(p, "r")
        click(s, x, y)
        s.settle(80)
        check("while a column split above the column floor still happens",
              len(s.panes()) == 2, str(len(s.panes())))
    os.unlink(conf)


def test_the_keyboard_obeys_the_same_floor():
    conf = cfg(FAST_TEXT + "min_split cols=40 rows=14\n")
    with Session(SH, cols=80, rows=26, config=conf) as s:
        s.settle()
        s.key("\\\\")
        s.settle(80)
        check("the keyboard will not split below the floor either",
              len(s.panes()) == 1, str(len(s.panes())))
        # ...but a script asking for a pane is declaring, not being offered.
        s.api("split", dir="cols")
        s.settle(80)
        check("the control API is not bound by it", len(s.panes()) == 2,
              str(len(s.panes())))
    os.unlink(conf)


def test_the_guide_says_which_way_it_goes():
    with Session(SH, cols=120, rows=26, config=FAST) as s:
        s.settle()
        p = s.pane()
        for side in ("l", "r", "t", "b"):
            x, y = edge_of(p, side)
            rest(s, x, y)
            body = s.snapshot().screen()
            check(f"the {side} guide carries its own arrow",
                  ARROWS[side] in body, repr(ARROWS[side]))
            others = [a for k, a in ARROWS.items() if k != side]
            check(f"and only that one", not any(o in body for o in others),
                  repr(body[:80]))


def test_the_arrow_sits_on_the_new_boundary():
    with Session(SH, cols=120, rows=26, config=FAST) as s:
        s.settle()
        p = s.pane()
        x, y = edge_of(p, "r")
        rest(s, x, y)
        snap = s.snapshot()
        pos = snap.find(ARROWS["r"])
        check("the arrow is on the dashed line, not the border",
              pos is not None and pos[0] == p["x"] + p["w"] // 2, str(pos))


if __name__ == "__main__":
    test_no_more_plus()
    test_each_border_splits_toward_itself()
    test_the_guide()
    test_guide_follows_the_new_layout()
    test_the_guide_waits_for_a_rest()
    test_press_skips_the_wait()
    test_delay_is_configurable()
    test_guide_is_per_pane()
    test_drag_still_moves()
    test_tiny_panes_have_no_targets()
    test_a_split_below_the_floor_is_not_offered()
    test_the_floor_is_configurable_in_both_axes()
    test_the_keyboard_obeys_the_same_floor()
    test_the_guide_says_which_way_it_goes()
    test_the_arrow_sits_on_the_new_boundary()
    sys.exit(report())
