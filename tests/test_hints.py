#!/usr/bin/env python3
"""A word in the middle of the status line for whatever the pointer is on.

The frame's affordances are mostly one character wide, and a border that splits
when clicked looks exactly like a border. The hint is read off the hit list
rather than tracked, so anything that registers a hit gets one by being named
in the table and nothing has to remember to raise it.
"""
import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def hover(s, x, y):
    s.send(rf"\e[<35;{x + 1};{y + 1}M")


def bar(s):
    return s.snapshot().text[-2]


def hit(s, prefix):
    return [h for h in s.snapshot().hits if h["action"].startswith(prefix)][0]


def test_each_button_says_what_it_does():
    with Session(SH, cols=96, rows=18) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        check("nothing is said before the pointer is on anything",
              "minimise" not in bar(s) and "close" not in bar(s), repr(bar(s)))

        for prefix, word in (("minimize:", "minimise"),
                             ("zoom:", "fill the tab"),
                             ("close:", "close this pane")):
            h = hit(s, prefix)
            hover(s, h["x"], h["y"])
            check(f"{prefix[:-1]} says its word", word in bar(s), repr(bar(s)))


def test_the_zoom_hint_says_which_way_it_goes():
    with Session(SH, cols=96, rows=18) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        h = hit(s, "zoom:")
        hover(s, h["x"], h["y"])
        check("it offers to fill the tab", "fill the tab" in bar(s), repr(bar(s)))

        s.send(r"\x01z")
        s.settle(20)
        h = hit(s, "zoom:")          # the button moved with the pane
        hover(s, h["x"], h["y"])
        check("and to undo it once it is zoomed",
              "back to the layout" in bar(s), repr(bar(s)))


def test_a_border_names_the_direction_it_would_split():
    with Session(SH, cols=96, rows=22) as s:
        s.settle(20)
        p = s.pane()
        for x, y, word in (
                (p["x"], p["y"] + 2, "split left"),
                (p["x"] + p["w"] - 1, p["y"] + 2, "split right"),
                (p["x"] + 6, p["y"] + p["h"] - 1, "split down")):
            hover(s, x, y)
            check(f"the border says {word}", word in bar(s), repr(bar(s)))


def test_the_gap_and_the_strip_and_the_bar():
    with Session(SH, cols=96, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        left = s.panes()[0]
        hover(s, left["x"] + left["w"], left["y"] + 3)
        check("the gap between panes offers a resize",
              "drag to resize" in bar(s), repr(bar(s)))

        t = hit(s, "tab:")
        hover(s, t["x"] + 1, t["y"])
        check("a tab offers to switch", "switch to this tab" in bar(s),
              repr(bar(s)))
        n = hit(s, "newtab")
        hover(s, n["x"] + 1, n["y"])
        check("+tab offers a new one", "new tab" in bar(s), repr(bar(s)))

        s.send(r"\x01m")
        s.settle(20)
        gone = [p for p in s.panes() if p["w"] <= 1][0]["id"]
        e = [h for h in s.snapshot().hits
             if h["action"] == f"focus:{gone}" and h["h"] == 1][0]
        hover(s, e["x"] + 1, e["y"])
        check("a pane put away offers to come back",
              "open this pane" in bar(s), repr(bar(s)))


def test_the_hint_never_writes_over_the_line_it_shares():
    with Session(SH, cols=96, rows=18) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        h = hit(s, "close:")
        hover(s, h["x"], h["y"])
        row = bar(s)
        left_end = len(row.rstrip()) and row.index("p")           # the pane name
        hint_at = row.index("close this pane")
        right_at = row.rindex("pane 1/2" if "pane 1/2" in row else "pane 2/2")
        check("the pane index on the right survives", right_at > 0, repr(row))
        check("and the hint sits between the two ends, touching neither",
              left_end < hint_at
              and hint_at + len("close this pane") < right_at, repr(row))

    # A long name leaves no room between the ends, so the hint is dropped
    # rather than drawn over the thing it would be explaining.
    wordy = ["/bin/sh", "-c",
             'printf "\\033]2;a-very-long-pane-title-indeed\\007"; '
             'stty raw -echo; cat']
    with Session(wordy, cols=60, rows=18) as s:
        s.settle(20)
        h = hit(s, "close:")
        hover(s, h["x"], h["y"])
        row = bar(s)
        check("with no room between them the hint gives way to the line",
              "close this pane" not in row
              and "a-very-long-pane-title-indeed" in row
              and "pane 1/1" in row, repr(row))


def test_hints_can_be_turned_off():
    conf = cfg("hints false\n")
    with Session(SH, cols=96, rows=18, config=conf) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        h = hit(s, "close:")
        hover(s, h["x"], h["y"])
        check("hints false says nothing", "close this pane" not in bar(s),
              repr(bar(s)))
    os.unlink(conf)


if __name__ == "__main__":
    test_each_button_says_what_it_does()
    test_the_zoom_hint_says_which_way_it_goes()
    test_a_border_names_the_direction_it_would_split()
    test_the_gap_and_the_strip_and_the_bar()
    test_the_hint_never_writes_over_the_line_it_shares()
    test_hints_can_be_turned_off()
    sys.exit(report())
