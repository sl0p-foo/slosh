#!/usr/bin/env python3
"""M5 / D6: responsive collapse as a pure function of the rect.

The fork this replaces had a bug where a pane added after a narrow->wide cycle
was absorbed into a stack at full width, because feasibility was checked
against a pane count that a stack lies about. Here the layout is recomputed
from the tree and the rect every frame, so the equivalent bug has nowhere to
live — and these tests are what says so.
"""
import os
import tempfile
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


# ---- collapsing flattens the hierarchy -------------------------------------

def nested_layout():
    """left column of three stacked panes, plus one on the right."""
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write('layout {\n tab name="t" {\n  pane split="rows" { pane\n'
            '   pane\n   pane }\n  pane\n }\n}\n')
    f.close()
    return f.name


def test_collapsing_flattens_the_tree():
    lay = nested_layout()
    with Session(SH, cols=50, rows=26, layout=lay) as s:
        s.settle(20)
        ids = [p["id"] for p in s.panes()]
        s.api("focus", id=ids[-1])
        s.settle(20)
        panes = s.panes()
        hidden = [p for p in panes if p["hidden"]]
        shown = [p for p in panes if not p["hidden"]]

        check("everything but the focused pane becomes a header",
              len(hidden) == 3 and len(shown) == 1, str(panes))
        check("each header is exactly one row",
              all(p["h"] == 1 for p in hidden), str([p["h"] for p in hidden]))
        # The nesting is what there is no room to express, so it stops
        # existing: no two panes sit side by side while collapsed.
        check("no two panes share a row",
              len({p["y"] for p in hidden}) == len(hidden),
              str([p["y"] for p in hidden]))
        check("the headers come first and the body below them",
              shown[0]["y"] > max(p["y"] for p in hidden), str(panes))
    os.unlink(lay)


def test_every_collapsed_pane_is_clickable():
    """The bug flattening fixes: a collapsed subtree used to give every pane
    inside it the same rect and one hit for the first of them."""
    lay = nested_layout()
    with Session(SH, cols=50, rows=26, layout=lay) as s:
        s.settle(20)
        ids = [p["id"] for p in s.panes()]
        s.api("focus", id=ids[-1])
        s.settle(20)
        snap = s.snapshot()
        hidden = [p for p in s.panes() if p["hidden"]]

        actions = [snap.hit_at(p["x"] + 2, p["y"]) for p in hidden]
        check("every header carries its own pane's focus action",
              actions == [f"focus:{p['id']}" for p in hidden], str(actions))
        check("and they are all different", len(set(actions)) == len(actions),
              str(actions))

        # Clicking the *last* header must reach that pane, not the first one.
        target = hidden[-1]
        s.click(target["x"] + 2, target["y"])
        s.settle(20)
        now = {p["id"]: p for p in s.panes()}
        check("clicking the last header expands that exact pane",
              not now[target["id"]]["hidden"] and now[target["id"]]["h"] > 1,
              str(s.panes()))
    os.unlink(lay)


def test_a_flattened_stack_offers_no_resize_handles():
    """Nothing is side by side any more, so there is nothing to drag."""
    lay = nested_layout()
    with Session(SH, cols=50, rows=26, layout=lay) as s:
        s.settle(20)
        ids = [p["id"] for p in s.panes()]
        s.api("focus", id=ids[-1])
        s.settle(20)
        snap = s.snapshot()
        edges = [h for h in snap.hits if h["action"].startswith("edge:")]
        check("a collapsed stack registers no resize edges", edges == [],
              str(edges))
    os.unlink(lay)


def test_a_tab_is_laid_out_or_it_is_a_list_never_both():
    """The whole tab flattens, not just the subtree that could not fit.

    Half a screen of panes next to half a screen of headers explains neither
    what happened nor what to do about it, so it is not a state we produce.
    """
    lay = nested_layout()
    # min_pane tuned so the *right column* alone cannot fit while the left
    # pane, on its own, comfortably could. Locally, only the right would
    # collapse; globally, everything does.
    conf = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    conf.write("min_pane cols=24 rows=17\n")
    conf.close()
    with Session(SH, cols=116, rows=52, config=conf.name, layout=lay) as s:
        s.settle(20)
        panes = s.panes()
        shown = [p for p in panes if not p["hidden"]]
        hidden = [p for p in panes if p["hidden"]]

        check("exactly one pane is open", len(shown) == 1, str(panes))
        check("every other pane is a header, including the one that did fit",
              len(hidden) == len(panes) - 1, str(panes))
        check("the headers all span the full width",
              len({p["w"] for p in hidden}) == 1
              and hidden[0]["w"] == shown[0]["w"],
              str([(p["id"], p["w"]) for p in panes]))
        # The give-away for the old behaviour: a full-height pane beside a
        # stack. Nothing may sit next to a header any more.
        check("no pane sits beside a header",
              all(p["x"] == hidden[0]["x"] for p in panes),
              str([(p["id"], p["x"]) for p in panes]))
    os.unlink(conf.name)
    os.unlink(lay)


# ---- hovering a row in the stack -------------------------------------------

BTN_BG = "#ff5fd7"


def stacked_session():
    """A tab small enough to be a list, with two headers and one body."""
    lay = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    lay.write('layout {\n tab name="t" {\n  pane\n'
              '  pane split="rows" { pane\n   pane }\n }\n}\n')
    lay.close()
    conf = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    conf.write("min_pane cols=24 rows=17\n")
    conf.close()
    return Session(SH, cols=100, rows=20, config=conf.name, layout=lay.name)


def hover(s, x, y):
    s.send(rf"\e[<35;{x + 1};{y + 1}M")


def row_bg(snap, pane):
    return (snap.style_at(pane["x"] + 4, pane["y"]) or {}).get("bg")


def test_hovering_a_row_lights_it_up():
    with stacked_session() as s:
        s.settle(20)
        headers = [p for p in s.panes() if p["hidden"]]
        check("the tab is a list with rows to hover", len(headers) >= 2,
              str(s.panes()))
        if len(headers) < 2:
            return

        snap = s.snapshot()
        check("nothing is lit before the pointer arrives",
              all(row_bg(snap, p) != BTN_BG for p in headers), "")

        hover(s, headers[0]["x"] + 4, headers[0]["y"])
        snap = s.snapshot()
        check("the row under the pointer is lit",
              row_bg(snap, headers[0]) == BTN_BG, str(row_bg(snap, headers[0])))
        check("and only that row",
              all(row_bg(snap, p) != BTN_BG for p in headers[1:]), "")

        hover(s, headers[1]["x"] + 4, headers[1]["y"])
        snap = s.snapshot()
        check("the light follows the pointer down the list",
              row_bg(snap, headers[1]) == BTN_BG
              and row_bg(snap, headers[0]) != BTN_BG, "")


def test_leaving_the_stack_puts_the_light_out():
    with stacked_session() as s:
        s.settle(20)
        headers = [p for p in s.panes() if p["hidden"]]
        body = [p for p in s.panes() if not p["hidden"]][0]
        if not headers:
            return
        hover(s, headers[0]["x"] + 4, headers[0]["y"])
        hover(s, body["content_x"] + 2, body["content_y"] + 2)
        snap = s.snapshot()
        check("no row is lit once the pointer is off the list",
              all(row_bg(snap, p) != BTN_BG for p in headers), "")


def test_hovering_a_row_does_not_open_it():
    """Feedback about where the pointer is, not an action. Opening on hover
    would make the list shuffle under the mouse as you read it."""
    with stacked_session() as s:
        s.settle(20)
        before = [p["id"] for p in s.panes() if p["focused"]]
        headers = [p for p in s.panes() if p["hidden"]]
        if not headers:
            return
        for h in headers:
            hover(s, h["x"] + 4, h["y"])
        s.settle(20)
        after = [p["id"] for p in s.panes() if p["focused"]]
        check("hovering every row changes nothing about which is open",
              before == after, f"{before} -> {after}")
        check("and they are all still headers",
              len([p for p in s.panes() if p["hidden"]]) == len(headers), "")

        # ...but clicking one still does open it.
        s.click(headers[-1]["x"] + 4, headers[-1]["y"])
        s.settle(20)
        now = {p["id"]: p for p in s.panes()}
        check("clicking the row it lit is what opens that pane",
              not now[headers[-1]["id"]]["hidden"], str(s.panes()))


if __name__ == "__main__":
    test_collapse()
    test_collapse_expand_cycle()
    test_focus_expands()
    test_click_a_header()
    test_hidden_panes_keep_running()
    test_tiny_terminal()
    test_collapsing_flattens_the_tree()
    test_every_collapsed_pane_is_clickable()
    test_a_flattened_stack_offers_no_resize_handles()
    test_a_tab_is_laid_out_or_it_is_a_list_never_both()
    test_hovering_a_row_lights_it_up()
    test_leaving_the_stack_puts_the_light_out()
    test_hovering_a_row_does_not_open_it()
    sys.exit(report())
