#!/usr/bin/env python3
"""Composition, styling, cursor, scrollback, resize, and the input round trip.

Everything here runs through the headless driver, so it is deterministic and
takes milliseconds. Content is addressed through the layout (`content_x/y`)
rather than assumed to start at 0,0 — panes have chrome, and a test that
hardcodes the offset is a test that breaks every time the chrome changes.
"""

import sys

from harness import Session, check, report

CAT = ["/bin/sh", "-c", "stty raw -echo; cat -v"]


def SH(script):
    return ["/bin/sh", "-c", script]


def test_text_and_cursor():
    with Session(SH("stty raw -echo; cat"), cols=30, rows=8) as s:
        s.settle()
        s.raw("hello")
        s.settle()
        p = s.pane()
        snap = s.snapshot()
        check(
            "text lands on the pane's first row",
            snap.pane_line(p, 0).startswith("hello"),
            repr(snap.pane_line(p, 0)),
        )
        check(
            "cursor follows the text",
            (snap.cursor["x"], snap.cursor["y"])
            == (p["content_x"] + 5, p["content_y"]),
            str(snap.cursor),
        )

        s.raw("\\r\\nsecond")
        s.settle()
        snap = s.snapshot()
        check(
            "newline moves to the next row",
            snap.pane_line(p, 1).startswith("second"),
            repr(snap.pane_line(p, 1)),
        )
        check(
            "cursor is on that row",
            snap.cursor["y"] == p["content_y"] + 1,
            str(snap.cursor),
        )


def test_styles():
    with Session(
        SH(r'printf "n\033[1mB\033[31mR\033[0mn"; sleep 5'), cols=30, rows=6
    ) as s:
        s.settle()
        p, snap = s.pane(), s.snapshot()
        cx, cy = p["content_x"], p["content_y"]
        check(
            "plain cell has no style run",
            snap.style_at(cx, cy) is None,
            str(snap.style_at(cx, cy)),
        )
        b = snap.style_at(cx + 1, cy)
        check("bold is recorded", b and b["attrs"] == ["bold"], str(b))
        r = snap.style_at(cx + 2, cy)
        check(
            "colour is resolved to rgb",
            r and r["fg"] is not None and "bold" in r["attrs"],
            str(r),
        )
        check(
            "reset ends the run",
            snap.style_at(cx + 3, cy) is None,
            str(snap.style_at(cx + 3, cy)),
        )


def test_wide_and_clusters():
    with Session(SH('printf "[\\346\\227\\245]"; sleep 5'), cols=30, rows=6) as s:
        s.settle()
        p, snap = s.pane(), s.snapshot()
        check(
            "wide char occupies one cell in the dump",
            snap.pane_line(p, 0).startswith("[日]"),
            repr(snap.pane_line(p, 0)),
        )

    with Session(
        SH('printf "[\\360\\237\\207\\263\\360\\237\\207\\261]"; sleep 5'),
        cols=30,
        rows=6,
    ) as s:
        s.settle()
        p, snap = s.pane(), s.snapshot()
        check(
            "a flag is one grapheme cluster, not two",
            snap.pane_line(p, 0).startswith("[🇳🇱]"),
            repr(snap.pane_line(p, 0)),
        )


def test_scrollback():
    with Session(SH("stty raw -echo; cat"), cols=30, rows=7) as s:
        s.settle()
        p = s.pane()
        n = p["content_h"]
        s.raw("\\r\\n".join(str(i) for i in range(1, n + 3)))
        s.settle()
        snap = s.snapshot()
        check(
            "viewport shows the tail, not the head",
            snap.pane_line(p, 0).startswith("3")
            and snap.pane_line(p, n - 1).startswith(str(n + 2)),
            repr(snap.pane_text(p)),
        )


def test_resize():
    with Session(SH("stty raw -echo; cat"), cols=30, rows=8) as s:
        s.settle()
        s.resize(50, 12)
        s.settle()
        snap, p = s.snapshot(), s.pane()
        check(
            "resize changes the composited size",
            snap.cols == 50 and snap.rows == 12 and len(snap.line(0)) == 50,
            f"{snap.cols}x{snap.rows}",
        )
        # chrome budget: gap 1 (x2 aspect => 2 cols) each side, a border, the
        # one-row tab strip along the top and the status line along the bottom
        check(
            "the pane grows with the screen, minus its chrome",
            p["content_w"] == snap.cols - 6 and p["content_h"] == snap.rows - 6,
            str(p),
        )
        check("pane still responds after resize", s.alive())


def test_input_round_trip():
    """The decoder and the per-pane encoder, without a pty in sight."""
    with Session(CAT, cols=50, rows=8) as s:
        s.settle()
        s.send(r"\e[1;5A")  # legacy ctrl-up
        s.settle()
        check("legacy key reaches the pane", "^[[1;5A" in s.snapshot().screen())

    with Session(CAT, cols=50, rows=8) as s:
        s.settle()
        s.send(r"\e[98;5u")  # kitty ctrl-b -> legacy pane
        s.settle()
        check(
            "kitty key is re-encoded for a legacy pane", "^B" in s.snapshot().screen()
        )

    with Session(CAT, cols=50, rows=8) as s:
        s.settle()
        s.send(r"\e[200~pasted\e[201~")
        s.settle()
        check(
            "paste is unwrapped for a pane that did not ask",
            "pasted" in s.snapshot().screen(),
        )

    kitty = [
        "/bin/sh",
        "-c",
        'stty raw -echo; printf "\\033[>1u\\033[?1000h\\033[?1006h"; cat -v',
    ]
    with Session(kitty, cols=50, rows=8) as s:
        s.settle()
        s.send(r"\x02")  # legacy ctrl-b -> kitty pane
        s.settle()
        check(
            "legacy key is re-encoded for a kitty pane",
            "^[[98;5u" in s.snapshot().screen(),
            s.snapshot().screen(),
        )


def test_terminal_replies():
    """WRITE_PTY: a query from the app must be answered on its own stdin.

    Regression: the callback options are pointer-typed, so passing &fn stored a
    stack address that jumped into a dead frame the moment pane_new returned.
    Nothing exercised it until a program asked the terminal a question.
    """
    with Session(SH('stty raw -echo; printf "\\033[6n"; cat -v'), cols=40, rows=8) as s:
        s.settle()
        out = s.snapshot().pane_text(s.pane())
        check(
            "device status report is answered",
            "R" in out and "^[[" in out,
            repr(out[:60]),
        )

    with Session(SH('printf "\\033]2;build.sh\\007"; sleep 5'), cols=44, rows=8) as s:
        s.settle()
        p = s.pane()
        check("OSC 2 sets the pane title", p["title"] == "build.sh", str(p["title"]))
        check(
            "the title is drawn in the frame",
            "build.sh" in s.snapshot().line(p["y"]),
            repr(s.snapshot().line(p["y"])),
        )


def test_hitlist_contract():
    with Session(SH("sleep 5"), cols=30, rows=6) as s:
        s.settle()
        p, snap = s.pane(), s.snapshot()
        check(
            "the pane body is hit-testable",
            snap.hit_at(p["content_x"], p["content_y"]) == f"pane:{p['id']}",
            str(snap.hit_at(p["content_x"], p["content_y"])),
        )
        check(
            "the gap ring belongs to nothing",
            snap.hit_at(0, 0) is None,
            str(snap.hit_at(0, 0)),
        )


if __name__ == "__main__":
    test_text_and_cursor()
    test_styles()
    test_wide_and_clusters()
    test_scrollback()
    test_resize()
    test_input_round_trip()
    test_terminal_replies()
    test_hitlist_contract()
    sys.exit(report())
