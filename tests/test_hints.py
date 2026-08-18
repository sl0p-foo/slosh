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
        check(
            "nothing is said before the pointer is on anything",
            "minimise" not in bar(s) and "close" not in bar(s),
            repr(bar(s)),
        )

        for prefix, word in (
            ("minimize:", "minimise"),
            ("zoom:", "fill the tab"),
            ("close:", "close this pane"),
        ):
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
        h = hit(s, "zoom:")  # the button moved with the pane
        hover(s, h["x"], h["y"])
        check(
            "and to undo it once it is zoomed",
            "back to the layout" in bar(s),
            repr(bar(s)),
        )


def test_a_border_names_the_direction_it_would_split():
    """The caption belongs to the handle, not the whole edge: the rest of an
    edge does nothing, and telling you it splits would be a lie about it."""
    with Session(SH, cols=96, rows=22) as s:
        s.settle(20)
        p = s.pane()
        for side, word in (
            ("l", "split left"),
            ("r", "split right"),
            ("b", "split down"),
            ("t", "split up"),
        ):
            x, y = s.snapshot().handle(p["id"], side)
            hover(s, x, y)
            check(f"the {side} handle says {word}", word in bar(s), repr(bar(s)))

        rx, ry = s.snapshot().rim(p["id"], "l")
        hover(s, rx, ry)
        check(
            "and the rest of the edge says nothing about splitting",
            "split" not in bar(s),
            repr(bar(s)),
        )


def test_the_gap_and_the_strip_and_the_bar():
    with Session(SH, cols=96, rows=20) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        left = s.panes()[0]
        hover(s, left["x"] + left["w"], left["y"] + 3)
        check(
            "the gap between panes offers a resize",
            "drag to resize" in bar(s),
            repr(bar(s)),
        )

        t = hit(s, "tab:")
        hover(s, t["x"] + 1, t["y"])
        check(
            "a tab offers the two things you would not guess",
            "rename" in bar(s) and "reorder" in bar(s),
            repr(bar(s)),
        )
        n = hit(s, "newtab")
        hover(s, n["x"] + 1, n["y"])
        check("the new-tab button offers a new one", "new tab" in bar(s), repr(bar(s)))

        s.send(r"\x01m")
        s.settle(20)
        gone = [p for p in s.panes() if p["w"] <= 1][0]["id"]
        e = [
            h
            for h in s.snapshot().hits
            if h["action"] == f"focus:{gone}" and h["h"] == 1
        ][0]
        hover(s, e["x"] + 1, e["y"])
        check(
            "a pane put away offers to come back",
            "open this pane" in bar(s),
            repr(bar(s)),
        )


def test_the_middle_is_centred_on_the_row_not_on_the_gap():
    """It used to be centred in whatever space the two ends left over, which
    meant it moved whenever a pane title or a state indicator changed length —
    the one thing on the line worth glancing at was never twice in the same
    place. It is centred on the row now, and the ends give way."""
    with Session(SH, cols=96, rows=18) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        h = hit(s, "close:")
        hover(s, h["x"], h["y"])
        row = bar(s)
        at = row.index("close this pane")
        want = (96 - len("close this pane")) // 2
        check(
            "the hint is centred on the terminal",
            abs(at - want) <= 1,
            f"at {at}, expected about {want}: {row!r}",
        )


def test_it_stays_put_when_the_ends_change_length():
    """The whole point of the change: a fixed position you can find without
    looking beats a tidy one you cannot."""
    seen = set()
    # Wide enough that all three names leave the middle room: this is about
    # the banner not *moving*, and a narrower terminal would be testing the
    # rule that drops it instead.
    for name in ("s", "medium-name", "a-really-quite-long-session-name"):
        with Session(SH, cols=120, rows=10) as s:
            s.settle(20)
            s.api("set-purpose", target="tab", id=s.tabs()[0]["id"], purpose=name)
            s.settle(20)
            row = bar(s)
            check(f"the banner is up with {name!r}", "sl0ppty 0." in row, row)
            seen.add(row.index("sl0ppty 0."))
    check("and in the same column every time", len(seen) == 1, str(seen))


def test_a_full_line_drops_the_middle_rather_than_overlapping():
    """The trade, stated the other way round from where it started: the ends
    say which session and pane you are in, and the middle is a hint you can
    get again by hovering or a version you can read a moment later. So the
    middle is the part that disappears."""
    wordy = [
        "/bin/sh",
        "-c",
        'printf "\\033]2;a-very-long-pane-title-indeed\\007"; stty raw -echo; cat',
    ]
    with Session(wordy, cols=60, rows=18) as s:
        s.settle(20)
        h = hit(s, "close:")
        hover(s, h["x"], h["y"])
        row = bar(s)
        check(
            "the hint is dropped when it would not fit clear of the ends",
            "close this pane" not in row,
            repr(row),
        )
        check(
            "and both ends survive intact",
            "a-very-long-pane-title-indeed" in row and "pane 1/1" in row,
            repr(row),
        )


def test_it_keeps_a_blank_column_on_each_side():
    """Not overlapping is not enough: touching reads as overlapping."""
    with Session(SH, cols=96, rows=18) as s:
        s.settle(20)
        row = bar(s)
        at = row.index("sl0ppty 0.")
        end = at + len(row[at:].split("  ")[0])
        check("a gap before it", row[at - 1] == " ", repr(row[at - 4 : at + 4]))
        check("a gap after it", row[end] == " ", repr(row[end - 4 : end + 4]))


def test_the_version_sits_in_the_slot_when_no_hint_does():
    """The middle of the status line is empty most of the time, and the first
    question about a misbehaving session is which build it is running — which
    is not "whatever was built last", because a session keeps the binary it
    started with."""
    with Session(SH, cols=96, rows=18) as s:
        s.settle(20)
        check("the banner is there by default", "sl0ppty 0." in bar(s), bar(s))

        s.api("split", dir="cols")
        s.settle(20)
        h = hit(s, "close:")
        hover(s, h["x"], h["y"])
        check("a hint takes the slot back", "close this pane" in bar(s), bar(s))
        check("and the banner gives way", "sl0ppty 0." not in bar(s), bar(s))

        hover(s, h["x"], h["y"] + 4)  # off the button, into the pane
        s.settle(20)
        check(
            "the banner returns when the pointer moves off",
            "sl0ppty 0." in bar(s),
            bar(s),
        )


def test_the_banner_and_the_hints_are_separate_knobs():
    off = cfg("version_banner false\n")
    with Session(SH, cols=96, rows=18, config=off) as s:
        s.settle(20)
        check("version_banner false says nothing", "sl0ppty" not in bar(s), bar(s))
        s.api("split", dir="cols")
        s.settle(20)
        h = hit(s, "close:")
        hover(s, h["x"], h["y"])
        check("but hints still work", "close this pane" in bar(s), bar(s))
    os.unlink(off)

    nohints = cfg("hints false\n")
    with Session(SH, cols=96, rows=18, config=nohints) as s:
        s.settle(20)
        check("hints false leaves the banner alone", "sl0ppty 0." in bar(s), bar(s))
        s.api("split", dir="cols")
        s.settle(20)
        h = hit(s, "close:")
        hover(s, h["x"], h["y"])
        check("and nothing replaces it", "sl0ppty 0." in bar(s), bar(s))
    os.unlink(nohints)


def test_hints_can_be_turned_off():
    conf = cfg("hints false\n")
    with Session(SH, cols=96, rows=18, config=conf) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        h = hit(s, "close:")
        hover(s, h["x"], h["y"])
        check("hints false says nothing", "close this pane" not in bar(s), repr(bar(s)))
    os.unlink(conf)


if __name__ == "__main__":
    test_each_button_says_what_it_does()
    test_the_zoom_hint_says_which_way_it_goes()
    test_a_border_names_the_direction_it_would_split()
    test_the_gap_and_the_strip_and_the_bar()
    test_the_middle_is_centred_on_the_row_not_on_the_gap()
    test_it_stays_put_when_the_ends_change_length()
    test_a_full_line_drops_the_middle_rather_than_overlapping()
    test_it_keeps_a_blank_column_on_each_side()
    test_hints_can_be_turned_off()
    test_the_version_sits_in_the_slot_when_no_hint_does()
    test_the_banner_and_the_hints_are_separate_knobs()
    sys.exit(report())
