#!/usr/bin/env python3
"""The bottom line: what it says, and that saying it cannot corrupt the stack.

`line[right - x] = 0` used a *width* as an index into a 256-byte buffer, so a
status line longer than the visible width on a terminal wider than the buffer
wrote a NUL past the end of a stack array -- one byte, which is why it crashed
the server now and then rather than every time. A session name plus a tab name
plus the title a program sets is enough to reach it, and a title is whatever
the program says it is.
"""

import sys

from harness import Session, check, report

LONG_TITLE = "T" * 250
# The pane announces a very long title, the way a shell with a deep path or an
# agent with a long task does, and then sits there.
CMD = [
    "/bin/sh",
    "-c",
    f"stty raw -echo; printf '\\033]2;{LONG_TITLE}\\007'; cat",
]


def status_row(snap):
    """The status line is the last row the frame draws on."""
    return snap.text[-1] if snap.text[-1].strip() else snap.text[-2]


def test_a_long_status_line_on_a_wide_terminal():
    """Wider than the buffer is the case that overflowed: 256 columns is not a
    special number to a terminal, and it was one to this code."""
    for cols in (80, 258, 300, 400):
        with Session(CMD, cols=cols, rows=10) as s:
            s.settle(60)
            s.api("set-name", target="tab", id=s.tabs()[0]["id"], name="N" * 60)
            s.settle(60)
            snap = s.snapshot()
            row = status_row(snap)
            check(
                f"the session survives drawing it at {cols} columns",
                s.alive(),
                f"died at cols={cols}",
            )
            check(
                f"and the line stays inside the screen at {cols}",
                len(row) <= cols,
                f"row is {len(row)} cells wide, screen is {cols}",
            )


def test_the_line_is_cut_to_the_room_there_is():
    """Narrow enough that the title cannot fit: it is cut, not wrapped, and not
    allowed to run into the right-hand block (`pane 1/1`)."""
    with Session(CMD, cols=60, rows=10) as s:
        s.settle(60)
        snap = s.snapshot()
        row = status_row(snap)
        check("the right-hand block is still there", "pane 1/1" in row, repr(row))
        titles = row.count("TTT")
        check("the title is on the line", titles >= 1, repr(row))
        check(
            "and it did not overwrite the block",
            row.index("pane 1/1") > row.index("TTT"),
            repr(row),
        )


def test_a_multibyte_title_is_cut_between_characters():
    """The cut walks whole UTF-8 sequences: a byte-indexed cut left half a
    character on the line, which the screen draws as a replacement glyph."""
    cmd = [
        "/bin/sh",
        "-c",
        "stty raw -echo; printf '\\033]2;%s\\007' "
        + "'" + ("\u65e5\u672c\u8a9e" * 40) + "'; cat",
    ]
    with Session(cmd, cols=60, rows=10) as s:
        s.settle(60)
        snap = s.snapshot()
        row = status_row(snap)
        check("no broken character on the line", "\ufffd" not in row, repr(row))
        check("the session is fine", s.alive())


if __name__ == "__main__":
    test_a_long_status_line_on_a_wide_terminal()
    test_the_line_is_cut_to_the_room_there_is()
    test_a_multibyte_title_is_cut_between_characters()
    sys.exit(report())
