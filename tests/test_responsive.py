#!/usr/bin/env python3
"""M5 / D6: responsive collapse as a pure function of the rect.

The fork this replaces had a bug where a pane added after a narrow->wide cycle
was absorbed into a stack at full width, because feasibility was checked
against a pane count that a stack lies about. Here the layout is recomputed
from the tree and the rect every frame, so the equivalent bug has nowhere to
live — and these tests are what says so.
"""
import sys

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;pane\\007"; stty raw -echo; cat']


def grid(s, n=4):
    """n panes in a mixed split, the arrangement that hurts on a small screen."""
    s.settle()
    s.key("\\\\")
    s.settle()
    s.key("\\\\")
    s.settle()
    s.key("-")
    s.settle()
    return s.panes()


def invariants(s, label):
    panes = [p for p in s.panes() if not p["hidden"]]
    snap = s.snapshot()
    rs = [(p["x"], p["y"], p["w"], p["h"]) for p in panes]
    for i in range(len(rs)):
        ax, ay, aw, ah = rs[i]
        if ax < 0 or ay < 0 or ax + aw > snap.cols or ay + ah > snap.rows:
            check(f"{label}: every visible pane is on screen", False, str(rs[i]))
            return panes
        for j in range(i + 1, len(rs)):
            bx, by, bw, bh = rs[j]
            if not (ax + aw <= bx or bx + bw <= ax or ay + ah <= by or by + bh <= ay):
                check(f"{label}: visible panes do not overlap", False,
                      f"{rs[i]} vs {rs[j]}")
                return panes
    check(f"{label}: {len(rs)} visible, no overlap, all on screen", True)
    return panes


def test_collapse():
    with Session(SH, cols=120, rows=32) as s:
        grid(s)
        check("nothing is hidden when there is room",
              not any(p["hidden"] for p in s.panes()), str(s.panes()))
        invariants(s, "roomy")

        s.resize(56, 16)
        s.settle()
        panes = s.panes()
        check("a squeezed layout collapses instead of shrinking to nothing",
              any(p["hidden"] for p in panes), str([p["hidden"] for p in panes]))
        invariants(s, "squeezed")

        snap = s.snapshot()
        check("the focused pane keeps a usable size",
              [p for p in panes if p["focused"]][0]["content_h"] >= 3,
              str([p for p in panes if p["focused"]]))
        check("collapsed panes are drawn as a header row",
              any(row.strip().startswith("pane") for row in snap.text),
              repr(snap.screen()))


def test_collapse_expand_cycle():
    """The fork's bug, made unrepresentable: state cannot go stale if there is none."""
    with Session(SH, cols=120, rows=32) as s:
        before = [(p["x"], p["y"], p["w"], p["h"]) for p in grid(s)]

        s.resize(50, 14)
        s.settle()
        check("narrow: something collapsed", any(p["hidden"] for p in s.panes()))

        s.resize(120, 32)
        s.settle()
        after = [(p["x"], p["y"], p["w"], p["h"]) for p in s.panes()]
        check("wide again: nothing is hidden",
              not any(p["hidden"] for p in s.panes()), str(s.panes()))
        check("the layout is exactly what it was before the cycle",
              before == after, f"{before}\n     != {after}")

        # the fork's actual symptom: a pane added after the cycle got stacked
        s.key("-")
        s.settle()
        panes = invariants(s, "after the cycle")
        check("a pane added after a narrow/wide cycle is not stacked",
              len(panes) == 5 and not any(p["hidden"] for p in s.panes()),
              str([(p["id"], p["hidden"]) for p in s.panes()]))


def test_focus_expands():
    with Session(SH, cols=120, rows=32) as s:
        grid(s)
        s.resize(50, 14)
        s.settle()
        hidden = [p for p in s.panes() if p["hidden"]]
        check("there is something to expand", hidden != [], str(s.panes()))
        if not hidden:
            return

        target = hidden[0]["id"]
        s.api("focus", id=target)
        s.settle()
        now = {p["id"]: p for p in s.panes()}
        check("focusing a collapsed pane expands it", not now[target]["hidden"],
              str(now[target]))
        check("and something else collapsed in its place",
              any(p["hidden"] for p in s.panes()))
        invariants(s, "after expanding")


def test_click_a_header():
    with Session(SH, cols=120, rows=32) as s:
        grid(s)
        s.resize(56, 16)
        s.settle()
        hidden = [p for p in s.panes() if p["hidden"]]
        if not hidden:
            check("there is a header to click", False)
            return
        h = hidden[0]
        snap = s.snapshot()
        action = snap.hit_at(h["x"] + 1, h["y"])
        check("a collapsed header is clickable", action == f"focus:{h['id']}",
              str(action))
        s.click(h["x"] + 1, h["y"])
        s.settle()
        check("clicking a header expands that pane",
              not [p for p in s.panes() if p["id"] == h["id"]][0]["hidden"])


def test_hidden_panes_keep_running():
    with Session(SH, cols=120, rows=32) as s:
        grid(s)
        first = s.panes()[0]
        s.api("focus", id=first["id"])
        s.settle()
        s.raw("written-before-collapse")
        s.settle()
        size_before = (first["content_w"], first["content_h"])

        # collapse it by focusing another pane and squeezing
        s.api("focus", id=s.panes()[-1]["id"])
        s.resize(50, 14)
        s.settle()
        now = {p["id"]: p for p in s.panes()}
        check("the pane we wrote to is hidden", now[first["id"]]["hidden"],
              str(now[first["id"]]))
        check("a hidden pane is not resized, so its program does not reflow",
              (now[first["id"]]["content_w"], now[first["id"]]["content_h"])
              == size_before,
              f"{size_before} -> {(now[first['id']]['content_w'], now[first['id']]['content_h'])}")

        s.resize(120, 32)
        s.api("focus", id=first["id"])
        s.settle()
        check("its content survived being collapsed",
              "written-before-collapse" in s.snapshot().screen(),
              repr(s.snapshot().screen()[:200]))


def test_tiny_terminal():
    """Nothing may crash or draw off-screen, however absurd the size."""
    with Session(SH, cols=120, rows=32) as s:
        grid(s)
        for cols, rows in [(40, 10), (24, 6), (12, 4), (80, 5), (200, 60), (60, 16)]:
            s.resize(cols, rows)
            s.settle(60)
            invariants(s, f"{cols}x{rows}")
        check("still alive after every size", s.api("alive")["alive"])


if __name__ == "__main__":
    test_collapse()
    test_collapse_expand_cycle()
    test_focus_expands()
    test_click_a_header()
    test_hidden_panes_keep_running()
    test_tiny_terminal()
    sys.exit(report())
