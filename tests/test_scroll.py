#!/usr/bin/env python3
"""Scrollback: the wheel, the keyboard, the indicator, and who owns the wheel.

The interesting part is not scrolling — libghostty-vt keeps the scrollback for
us — it is deciding whose wheel it is. A program tracking the mouse gets the
events; a full-screen program without mouse tracking has no scrollback of ours
to show, so the wheel means arrow keys to it; everything else scrolls us.
"""

import sys

from harness import Session, check, report

# a shell whose output we can generate on demand
SH = ["/bin/sh", "-c", "stty raw -echo; sh"]


def wheel(s, x, y, up=True):
    s.send(rf"\e[<{64 if up else 65};{x + 1};{y + 1}M")


def top_row(s, p=None):
    p = p or s.pane()
    return s.snapshot().pane_line(p, 0)


def fill(s, n=60):
    s.raw(f"seq 1 {n}\\n")
    s.settle(150)


def test_wheel_scrolls():
    with Session(SH, cols=50, rows=12) as s:
        s.settle()
        fill(s)
        p = s.pane()
        check(
            "the viewport is at the bottom",
            "60" in s.snapshot().pane_text(p),
            repr(s.snapshot().pane_text(p)),
        )

        wheel(s, p["content_x"] + 2, p["content_y"] + 2, up=True)
        s.settle(80)
        after = s.snapshot().pane_text(p)
        check("the wheel scrolls back", "60" not in after, repr(after))

        wheel(s, p["content_x"] + 2, p["content_y"] + 2, up=False)
        s.settle(80)
        check(
            "and forward again",
            "60" in s.snapshot().pane_text(p),
            repr(s.snapshot().pane_text(p)),
        )


def test_indicator():
    with Session(SH, cols=50, rows=12) as s:
        s.settle()
        fill(s)
        p = s.pane()
        snap = s.snapshot()
        check(
            "no indicator when the viewport is live",
            "▲" not in snap.line(p["y"]),
            repr(snap.line(p["y"])),
        )

        for _ in range(3):
            wheel(s, p["content_x"] + 2, p["content_y"] + 2, up=True)
        s.settle(80)
        snap = s.snapshot()
        row = snap.line(p["y"])
        # A space between the arrow and the count: U+25B2 is drawn wide enough
        # in plenty of fonts to touch what follows it.
        check(
            "scrolling shows how far down the live view is",
            "\u25b2 9" in row,
            repr(row),
        )

        x = row.index("▲")
        check(
            "the indicator is clickable",
            snap.hit_at(x, p["y"]) == f"scrollbottom:{p['id']}",
            str(snap.hit_at(x, p["y"])),
        )
        s.click(x, p["y"])
        s.settle(80)
        snap = s.snapshot()
        check(
            "clicking it returns to the live view",
            "▲" not in snap.line(p["y"]) and "60" in snap.pane_text(p),
            repr(snap.line(p["y"])),
        )


def test_keyboard():
    with Session(SH, cols=50, rows=12) as s:
        s.settle()
        fill(s)
        p = s.pane()
        s.send(r"\x01\e[5~")  # C-a PageUp
        s.settle(80)
        check(
            "C-a PageUp scrolls a page",
            "60" not in s.snapshot().pane_text(p),
            repr(s.snapshot().pane_text(p)),
        )

        s.send(r"\x01\e[6~")  # C-a PageDown
        s.settle(80)
        check("C-a PageDown comes back", "60" in s.snapshot().pane_text(p))

        s.send(r"\x01\e[H")  # C-a Home
        s.settle(80)
        text = s.snapshot().pane_text(p)
        # the top of the scrollback is the session's very first line, which is
        # the shell's first prompt — not the first line of `seq`
        check(
            "C-a Home goes to the top of the scrollback",
            "sh-5.3$" in text and "60" not in text,
            repr(text[:120]),
        )

        s.send(r"\x01\e[F")  # C-a End
        s.settle(80)
        check("C-a End returns to the bottom", "60" in s.snapshot().pane_text(p))


def test_typing_returns_to_the_live_view():
    with Session(SH, cols=50, rows=12) as s:
        s.settle()
        fill(s)
        p = s.pane()
        s.send(r"\x01\e[5~")
        s.settle(80)
        check("scrolled away", "60" not in s.snapshot().pane_text(p))

        s.send("echo back")  # typed, not run: the snap-back is what we assert
        s.settle(120)
        text = s.snapshot().pane_text(p)
        check("typing snaps back to the bottom", "60" in text, repr(text[-80:]))
        check(
            "and the indicator goes with it",
            "▲" not in s.snapshot().line(p["y"]),
            repr(s.snapshot().line(p["y"])),
        )


def test_scroll_position_is_per_pane():
    with Session(SH, cols=90, rows=14) as s:
        s.settle()
        fill(s, 40)
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        s.api("focus", id=left["id"])
        s.send(r"\x01\e[5~")
        s.settle(80)
        snap = s.snapshot()

        # side-by-side panes share a screen row, so read each one's columns
        def frame_of(pane):
            return snap.line(pane["y"])[pane["x"] : pane["x"] + pane["w"]]

        check("one pane is scrolled", "▲" in frame_of(left), repr(frame_of(left)))
        check("the other is not", "▲" not in frame_of(right), repr(frame_of(right)))


def test_who_owns_the_wheel():
    # a full-screen program with no mouse tracking: the wheel means arrows
    alt = ["/bin/sh", "-c", 'stty raw -echo; printf "\\033[?1049h"; cat -v']
    with Session(alt, cols=60, rows=12) as s:
        s.settle()
        p = s.pane()
        wheel(s, p["content_x"] + 2, p["content_y"] + 2, up=True)
        s.settle(80)
        out = s.snapshot().pane_text(p)
        check(
            "on the alternate screen the wheel becomes arrow keys",
            out.count("^[[A") == 3,
            repr(out[:120]),
        )
        check(
            "and does not scroll us instead",
            "▲" not in s.snapshot().line(p["y"]),
            repr(s.snapshot().line(p["y"])),
        )

    # a program tracking the mouse gets the wheel events themselves
    tracker = [
        "/bin/sh",
        "-c",
        'stty raw -echo; printf "\\033[?1000h\\033[?1006h"; cat -v',
    ]
    with Session(tracker, cols=60, rows=12) as s:
        s.settle()
        p = s.pane()
        wheel(s, p["content_x"] + 2, p["content_y"] + 2, up=True)
        s.settle(80)
        out = s.snapshot().pane_text(p)
        check(
            "a mouse-tracking program gets the wheel",
            "^[[<64;3;3M" in out,
            repr(out[:120]),
        )


def test_output_while_scrolled():
    with Session(SH, cols=50, rows=12) as s:
        s.settle()
        fill(s, 60)
        p = s.pane()
        s.send(r"\x01\e[5~")
        s.settle(80)
        before = s.snapshot().pane_line(p, 0)

        s.raw("echo NEWLINE\\n")
        s.settle(150)
        after = s.snapshot()
        check(
            "new output does not yank the viewport back",
            after.pane_line(p, 0) == before,
            f"{before!r} -> {after.pane_line(p, 0)!r}",
        )
        check(
            "but the indicator shows the gap growing",
            "▲" in after.line(p["y"]),
            repr(after.line(p["y"])),
        )


if __name__ == "__main__":
    test_wheel_scrolls()
    test_indicator()
    test_keyboard()
    test_typing_returns_to_the_live_view()
    test_scroll_position_is_per_pane()
    test_who_owns_the_wheel()
    test_output_while_scrolled()
    sys.exit(report())
