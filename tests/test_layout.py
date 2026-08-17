#!/usr/bin/env python3
"""M1: the layout tree, splits, focus, frames, and the hit list.

The layout is a pure function of the tree and the rect, so these assertions are
about *invariants* (no overlap, no drift, everything on screen) rather than
remembered geometry. That is the whole point of D6.
"""

import sys

from harness import Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]


def rects(panes):
    return [(p["x"], p["y"], p["w"], p["h"]) for p in panes]


def overlaps(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return not (ax + aw <= bx or bx + bw <= ax or ay + ah <= by or by + bh <= ay)


def check_invariants(name, s, snap=None):
    panes = s.panes()
    snap = snap or s.snapshot()
    rs = rects(panes)
    for i in range(len(rs)):
        for j in range(i + 1, len(rs)):
            if overlaps(rs[i], rs[j]):
                check(f"{name}: panes do not overlap", False, f"{rs[i]} vs {rs[j]}")
                return panes
    on_screen = all(
        x >= 0 and y >= 0 and x + w <= snap.cols and y + h <= snap.rows
        for x, y, w, h in rs
    )
    check(f"{name}: {len(rs)} panes, no overlap, all on screen", on_screen, str(rs))
    return panes


def test_split_and_close():
    # Big enough for both splits to clear min_split (32x8 by default): a column
    # split needs 2*32+2 columns of pane, a row split 2*8+1 rows.
    with Session(SH, cols=80, rows=24) as s:
        s.settle()
        check("starts with one pane", len(s.panes()) == 1)

        s.key("\\\\")  # C-a \  -> split into columns
        s.settle()
        panes = check_invariants("after vsplit", s)
        check(
            "vsplit makes two columns",
            len(panes) == 2
            and panes[0]["y"] == panes[1]["y"]
            and panes[0]["x"] != panes[1]["x"],
            str(rects(panes)),
        )
        check("the new pane takes focus", panes[1]["focused"])

        s.key("-")  # split the focused pane into rows
        s.settle()
        panes = check_invariants("after hsplit", s)
        check(
            "hsplit stacks inside the right column",
            panes[1]["x"] == panes[2]["x"] and panes[1]["y"] != panes[2]["y"],
            str(rects(panes)),
        )

        s.key("x")  # close the focused pane
        s.settle()
        panes = check_invariants("after close", s)
        check("close removes a pane", len(panes) == 2, str(len(panes)))

        s.key("x")
        s.settle()
        panes = check_invariants("after second close", s)
        check(
            "closing collapses the split back to one pane",
            len(panes) == 1 and panes[0]["w"] > 40,
            str(rects(panes)),
        )


def test_three_way_split_is_even():
    """Splitting the same direction twice must give equal shares, not 1/2+1/4+1/4."""
    with Session(SH, cols=110, rows=10) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        s.key("\\\\")
        s.settle()
        panes = check_invariants("three columns", s)
        widths = sorted(p["w"] for p in panes)
        check(
            "three splits give three even columns",
            len(panes) == 3 and widths[-1] - widths[0] <= 1,
            str(widths),
        )


def test_focus_movement():
    with Session(SH, cols=76, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        right = s.focused()
        s.key("h")  # focus left
        s.settle()
        left = s.focused()
        check(
            "focus moves left",
            left["id"] != right["id"] and left["x"] < right["x"],
            f"{left['id']} vs {right['id']}",
        )

        s.key("l")
        s.settle()
        check("focus moves back right", s.focused()["id"] == right["id"])

        s.key("k")  # nothing above: focus must not move or vanish
        s.settle()
        check("focus survives a move with no target", s.focused()["id"] == right["id"])

        s.key("o")
        s.settle()
        check("focus next cycles", s.focused()["id"] != right["id"])


def test_frames_and_gap():
    with Session(SH, cols=40, rows=8) as s:
        s.settle()
        snap = s.snapshot()
        pane = s.pane()
        check(
            "frame is drawn",
            "╭" in snap.line(pane["y"]) and "╯" in snap.line(pane["y"] + pane["h"] - 1),
            repr(snap.line(pane["y"])),
        )
        check("gap ring is blank", snap.line(0).strip() == "", repr(snap.line(0)))
        check(
            "content sits inside the frame",
            pane["content_x"] == pane["x"] + 1 and pane["content_y"] == pane["y"] + 1,
            str(pane),
        )

    with Session(SH, cols=76, rows=12) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        panes = s.panes()
        gap = panes[1]["x"] - (panes[0]["x"] + panes[0]["w"])
        check("horizontal gap is aspect-corrected (2 columns)", gap == 2, str(gap))


def test_focus_is_visible():
    with Session(SH, cols=76, rows=10) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        snap = s.snapshot()
        a, b = s.panes()
        style_a = snap.style_at(a["x"], a["y"])
        style_b = snap.style_at(b["x"], b["y"])
        check(
            "focused and unfocused frames differ",
            style_a and style_b and style_a["fg"] != style_b["fg"],
            f"{style_a} vs {style_b}",
        )


def test_hit_list():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        snap = s.snapshot()
        pane = s.pane()

        check(
            "pane body registers a hit",
            snap.hit_at(pane["content_x"], pane["content_y"]) == f"pane:{pane['id']}",
            str(snap.hit_at(pane["content_x"], pane["content_y"])),
        )

        # the border is the split target now; the top row is also the drag
        # handle, which is why it reports title: rather than border:
        check(
            "the top border is the drag handle",
            snap.hit_at(pane["x"] + 4, pane["y"]) == f"title:{pane['id']}",
            str(snap.hit_at(pane["x"] + 4, pane["y"])),
        )
        check(
            "the side border is a split target",
            snap.hit_at(pane["x"], pane["y"] + 2) == f"border:{pane['id']}:l",
            str(snap.hit_at(pane["x"], pane["y"] + 2)),
        )
        check(
            "the bottom border too",
            snap.hit_at(pane["x"] + 4, pane["y"] + pane["h"] - 1)
            == f"border:{pane['id']}:b",
            str(snap.hit_at(pane["x"] + 4, pane["y"] + pane["h"] - 1)),
        )


def test_click_focuses_and_forwards():
    with Session(SH, cols=76, rows=12) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        check("right pane has focus", right["focused"])

        s.click(left["content_x"] + 1, left["content_y"] + 1)
        s.settle()
        check("clicking a pane focuses it", s.focused()["id"] == left["id"])

    mouse = [
        "/bin/sh",
        "-c",
        'stty raw -echo; printf "\\033[?1000h\\033[?1006h"; cat -v',
    ]
    with Session(mouse, cols=60, rows=12) as s:
        s.settle()
        pane = s.pane()
        s.click(pane["content_x"] + 3, pane["content_y"] + 2)
        s.settle()
        # the pane must see coordinates relative to itself, not the screen
        check(
            "mouse is translated into pane-local coordinates",
            "^[[<0;4;3M" in s.snapshot().pane_text(pane),
            repr(s.snapshot().pane_text(pane)[:60]),
        )


def test_resize_keeps_invariants():
    with Session(SH, cols=80, rows=20) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        s.key("-")
        s.settle()
        for cols, rows in [(120, 30), (40, 10), (61, 13), (80, 20)]:
            s.resize(cols, rows)
            s.settle(60)
            check_invariants(f"at {cols}x{rows}", s)


if __name__ == "__main__":
    test_split_and_close()
    test_three_way_split_is_even()
    test_focus_movement()
    test_frames_and_gap()
    test_focus_is_visible()
    test_hit_list()
    test_click_focuses_and_forwards()
    test_resize_keeps_invariants()
    sys.exit(report())
