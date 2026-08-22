#!/usr/bin/env python3
"""Zooming a pane, and the buttons in the frame that do it.

Zoom is intent, not a derived state: nothing about the tree says a pane fills
its tab, you said so. So the things worth checking are that it survives the
layout being recomputed, that it cannot outlive the pane it names, and that it
is per tab.
"""

import os
import re
import subprocess
import sys
import tempfile

from harness import BIN, Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']


def mark(name):
    """A chrome mark as the binary currently draws it. Read from
    `--dump-config`, which is generated from the code, rather than written down
    here -- a test that pins a glyph starts failing the day somebody picks a
    nicer one, and says nothing useful when it does."""
    out = subprocess.run([BIN, "--dump-config"], capture_output=True, text=True).stdout
    m = re.search(name + r' "(.+)"', out)
    return m.group(1) if m else "?"


ZOOM, ZOOM_ON = mark("zoom_mark"), mark("zoom_on_mark")


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def visible(s):
    """Visible panes *in the current tab*.

    Only the current tab is laid out, so a pane in any other one carries
    whatever rect and hidden flag it had when its tab was last on screen --
    stale by design, and not an answer to "is it visible now"."""
    active = [t["index"] for t in s.tabs() if t["active"]][0]
    return [p for p in s.panes() if p["tab"] == active and not p["hidden"]]


def button(s, verb, pane_id=None):
    """The button's *current* rect. Zooming moves it, because the pane it
    belongs to just changed size."""
    if pane_id is None:
        pane_id = [p for p in s.panes() if p["focused"]][0]["id"]
    hits = [h for h in s.snapshot().hits if h["action"] == f"{verb}:{pane_id}"]
    return hits[0] if hits else None


def test_zoom_fills_the_tab_and_puts_it_back():
    with Session(SH, cols=90, rows=18) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        check("two panes to start", len(visible(s)) == 2, str(s.panes()))

        s.send(r"\x01z")
        s.settle(20)
        check("zooming leaves one pane on screen", len(visible(s)) == 1, str(s.panes()))
        check("but does not close the other", len(s.panes()) == 2, str(s.panes()))
        check(
            "and the zoomed pane has the whole tab",
            visible(s)[0]["w"] > 80,
            str(visible(s)[0]),
        )

        s.send(r"\x01z")
        s.settle(20)
        check("unzooming puts them both back", len(visible(s)) == 2, str(s.panes()))


def test_the_button_toggles_and_says_which_way():
    with Session(SH, cols=90, rows=18) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)

        b = button(s, "zoom")
        check("a pane's frame carries a zoom button", b is not None, "")
        if not b:
            return
        s.click(b["x"] + 1, b["y"])
        s.settle(20)
        check("clicking it zooms", len(visible(s)) == 1, str(s.panes()))
        check(
            "and the mark says so",
            ZOOM_ON in s.snapshot().line(2),
            repr(s.snapshot().line(2)[-14:]),
        )

        # The button moved: the pane it belongs to just became the whole tab.
        b = button(s, "zoom")
        s.click(b["x"] + 1, b["y"])
        s.settle(20)
        check("clicking it again puts it back", len(visible(s)) == 2, str(s.panes()))
        check(
            "and the mark goes back too",
            ZOOM in s.snapshot().line(2),
            repr(s.snapshot().line(2)[-14:]),
        )


def test_the_close_button_closes_that_pane():
    with Session(SH, cols=90, rows=18) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        panes = s.panes()
        victim = panes[0]["id"]
        b = button(s, "close", victim)
        check("a pane's frame carries a close button", b is not None, "")
        if not b:
            return
        s.click(b["x"] + 1, b["y"])
        s.settle(20)
        left = [p["id"] for p in s.panes()]
        check(
            "it closes the pane whose frame it is on",
            victim not in left and len(left) == 1,
            str(left),
        )


def test_a_zoom_cannot_outlive_its_pane():
    with Session(SH, cols=90, rows=18) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.send(r"\x01z")
        s.settle(20)
        check("zoomed", len(visible(s)) == 1, str(s.panes()))

        s.api("close", id=[p for p in s.panes() if p["focused"]][0]["id"])
        s.settle(20)
        check(
            "closing the zoomed pane leaves the tab usable",
            len(visible(s)) == 1 and len(s.panes()) == 1,
            str(s.panes()),
        )
        check(
            "and the survivor is not stuck hidden",
            not s.panes()[0]["hidden"],
            str(s.panes()),
        )


def test_zoom_is_per_tab():
    lay = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    lay.write(
        'layout {\n tab name="a" {\n  pane\n  pane\n }\n'
        ' tab name="b" {\n  pane\n  pane\n }\n}\n'
    )
    lay.close()
    with Session(SH, cols=90, rows=18, layout=lay.name) as s:
        s.settle(20)
        s.send(r"\x01z")
        s.settle(20)
        check("tab a is zoomed", len(visible(s)) == 1, str(s.panes()))

        tabs = s.tabs()
        s.api("select-tab", id=tabs[1]["id"])
        s.settle(20)
        check(
            "tab b is not, because zoom belongs to a tab",
            len(visible(s)) == 2,
            str(s.panes()),
        )

        s.api("select-tab", id=tabs[0]["id"])
        s.settle(20)
        check(
            "and tab a is still zoomed when you come back",
            len(visible(s)) == 1,
            str(s.panes()),
        )
    os.unlink(lay.name)


def test_the_status_line_says_zoomed():
    with Session(SH, cols=90, rows=18) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        check(
            "nothing said while it is not",
            "zoomed" not in s.snapshot().text[-2],
            repr(s.snapshot().text[-2]),
        )
        s.send(r"\x01z")
        s.settle(20)
        check(
            "a zoomed pane says so",
            "zoomed" in s.snapshot().text[-2],
            repr(s.snapshot().text[-2]),
        )


def test_the_buttons_can_be_turned_off():
    conf = cfg("pane_buttons false\n")
    with Session(SH, cols=90, rows=18, config=conf) as s:
        s.settle(20)
        hits = [
            h for h in s.snapshot().hits if h["action"].startswith(("zoom:", "close:"))
        ]
        check("pane_buttons false draws none", hits == [], str(hits))
        # ...but the verb is still there from the keyboard: the setting is
        # about the affordance, not about zooming.
        s.send(r"\x01z")
        s.settle(20)
        check(
            "while the keybinding still zooms",
            "zoomed" in s.snapshot().text[-2],
            repr(s.snapshot().text[-2]),
        )
    os.unlink(conf)


def test_a_new_pane_unzooms():
    """A pane arriving in a zoomed tab would land behind the pane filling it,
    which looks exactly like the split having failed — so asking for a new
    pane implicitly asks for the layout back."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.send(r"\x01z")
        s.settle(20)
        check("zoomed: one pane on screen", len(visible(s)) == 1, str(visible(s)))
        s.send(r"\x01-")
        s.settle(30)
        vis = visible(s)
        check("a split unzooms", len(vis) == 3, str(vis))
        check(
            "and the status stops saying so",
            "zoomed" not in s.snapshot().text[-2],
            repr(s.snapshot().text[-2]),
        )
        check(
            "the new pane holds focus",
            [p for p in vis if p["focused"]] != [],
            str(vis),
        )


def test_a_new_float_unzooms():
    """Floats are hidden while a tab is zoomed, so a new float opened over a
    zoomed tab would be invisible on arrival. Same rule, same fix."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.send(r"\x01z")
        s.settle(20)
        r = s.api("new-float")
        s.settle(30)
        check("the float opened", r.get("ok") and r.get("id"), str(r))
        fl = [p for p in visible(s) if p["floating"] and p["w"] > 1]
        check("and is on screen, not behind a zoom", len(fl) == 1, str(visible(s)))
        check(
            "the zoom is gone",
            "zoomed" not in s.snapshot().text[-2],
            repr(s.snapshot().text[-2]),
        )


def test_minimising_the_zoomed_pane_unzooms():
    """A zoom naming a minimised pane would solo a pane the strip holds —
    filling the tab and put away at once. Minimising it ends the zoom."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.send(r"\x01z")
        s.settle(20)
        check("zoomed over three panes", len(visible(s)) == 1, str(visible(s)))
        s.send(r"\x01m")
        s.settle(30)
        vis = [p for p in visible(s) if p["w"] > 1]
        check("the two others lay out again", len(vis) == 2, str(visible(s)))
        check(
            "and the status stops saying zoomed",
            "zoomed" not in s.snapshot().text[-2],
            repr(s.snapshot().text[-2]),
        )


if __name__ == "__main__":
    test_zoom_fills_the_tab_and_puts_it_back()
    test_the_button_toggles_and_says_which_way()
    test_the_close_button_closes_that_pane()
    test_a_zoom_cannot_outlive_its_pane()
    test_zoom_is_per_tab()
    test_the_status_line_says_zoomed()
    test_the_buttons_can_be_turned_off()
    test_a_new_pane_unzooms()
    test_a_new_float_unzooms()
    test_minimising_the_zoomed_pane_unzooms()
    sys.exit(report())
