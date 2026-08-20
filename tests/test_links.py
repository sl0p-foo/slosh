#!/usr/bin/env python3
"""OSC 8 hyperlinks pass through the compositor.

lib-vt tracks a hyperlink per cell; without re-emission the client's terminal
is never told, so `ls --hyperlink` and friends silently lose their links
inside the session. The link rides beside the cell (cell_t is the shader ABI
and a link is not a colour), the diff wraps emitted runs in OSC 8, and chrome
painted over a link takes the link with it.

Plain-text URLs are deliberately not slosh's job: the outer terminal's own
matcher runs over what slosh paints. (Under Ghostty, add shift to the usual
gesture while a mouse-owning app runs -- shift is its mouse-capture escape.)
"""

import sys

from harness import Session, check, report

URI = "https://sl0p.foo/x"
SH = [
    "/bin/sh",
    "-c",
    'printf "\\033]8;;' + URI + '\\033\\\\click me\\033]8;;\\033\\\\ done\\n";'
    " stty raw -echo; cat",
]


def links(s):
    return s.snapshot().data["links"]


def test_a_hyperlink_lands_on_the_cells_that_carry_it():
    with Session(SH, cols=80, rows=20) as s:
        s.until_text("click me")
        ls = links(s)
        check("one link run", len(ls) == 1, str(ls))
        if not ls:
            return
        run = ls[0]
        check("with the URI intact", run["uri"] == URI, str(run))
        check(
            "covering exactly the linked text",
            run["w"] == len("click me"),
            str(run),
        )
        snap = s.snapshot()
        check(
            "on the cells that show it",
            snap.line(run["y"])[run["x"] : run["x"] + run["w"]] == "click me",
            repr(snap.line(run["y"])),
        )


def test_the_emitted_bytes_wrap_the_run_in_osc8():
    """The model and the bytes are two different things, and this session
    already paid once for asserting on one while the other was wrong."""
    with Session(SH, cols=80, rows=20) as s:
        s.until_text("click me")
        b = s.api("snapshot", format="bytes")["bytes"]
        check(
            "the frame opens the link before its text",
            f"\x1b]8;;{URI}\x1b\\" in b,
            repr(b[-400:]),
        )
        opens = b.count("\x1b]8;;h")  # an open names a scheme
        closes = b.count("\x1b]8;;\x1b\\")
        check("and closes what it opens", opens == closes == 1, f"{opens}/{closes}")

        delta = s.api("snapshot", format="bytes")["bytes"]
        check("an unchanged frame emits nothing", delta == "", repr(delta[:80]))


def test_chrome_over_a_link_is_chrome():
    """Anything painted over a link takes the link with it: a modal across a
    URL must not leave its own box clickable with the pane's link."""
    with Session(SH, cols=80, rows=24) as s:
        s.until_text("click me")
        run = links(s)[0]
        s.send(r"\x01?")  # the cheatsheet: an opaque box over the layout
        s.settle(40)
        snap = s.snapshot()
        covered = snap.line(run["y"])[run["x"] : run["x"] + run["w"]] != "click me"
        check("the modal covers the linked text", covered, repr(snap.line(run["y"])))
        check(
            "and carries no link over it",
            snap.data["links"] == [],
            str(snap.data["links"]),
        )
        s.send("q")
        s.settle(40)
        check("dismissed, the link is back", len(links(s)) == 1, str(links(s)))


def test_a_link_scrolled_away_offers_nothing():
    """Only what is on screen can be clicked: the viewport is the promise,
    and scrollback brings the link back with the text that carries it."""
    with Session(SH, cols=80, rows=12) as s:
        s.until_text("click me")
        check("the link is there", len(links(s)) == 1, str(links(s)))
        # Push it out of the viewport: cat echoes what raw feeds it.
        s.raw(r"\n" * 30)
        s.until(lambda snap: "click me" not in snap.screen())
        check("scrolled away, no link", links(s) == [], str(links(s)))
        s.send(r"\x01\e[1~")  # C-a Home: to the oldest line
        snap = s.until_text("click me")
        check(
            "and back with the text, in scrollback",
            len(snap.data["links"]) == 1,
            str(snap.data["links"]),
        )


if __name__ == "__main__":
    test_a_hyperlink_lands_on_the_cells_that_carry_it()
    test_the_emitted_bytes_wrap_the_run_in_osc8()
    test_chrome_over_a_link_is_chrome()
    test_a_link_scrolled_away_offers_nothing()
    sys.exit(report())
