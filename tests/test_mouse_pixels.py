#!/usr/bin/env python3
"""SGR-pixel mouse (mode 1016) must agree with the size we report.

A pane is told how big a cell is twice: in mode 2048's resize report and in the
XTWINOPS answers. If mouse reports are encoded against a *different* cell size,
every click in a program that turned on 1016 lands somewhere else -- and a
program can turn it on by itself: Textual does, the moment in-band resize works,
and then divides each coordinate by the pixels-per-cell we advertised. A click
30 cells across came out as 3.

So the check is a relationship, not a number: whatever cell size the pane is
told about, the pixel coordinates it is sent must divide back down to the cell
that was clicked.
"""

import re
import sys

from harness import Session, check, report

# Enable in-band resize + SGR mouse, then echo everything the terminal sends.
PROBE = [
    "/bin/sh",
    "-c",
    'stty raw -echo; printf "\\033[?2048h\\033[?1000h\\033[?1006h"; %s cat -v',
]


def probe(pixels):
    argv = list(PROBE)
    argv[2] = argv[2] % ('printf "\\033[?1016h";' if pixels else "")
    return argv


def reported(out):
    """(cols, rows, cell_w, cell_h) from the mode-2048 report on screen."""
    m = re.search(r"\^\[\[48;(\d+);(\d+);(\d+);(\d+)t", out)
    if not m:
        return None
    rows, cols, pix_h, pix_w = (int(g) for g in m.groups())
    return cols, rows, pix_w // cols, pix_h // rows


def clicked(out):
    """(x, y) of the last SGR report on screen, 0-based."""
    hits = re.findall(r"\^\[\[<(\d+);(\d+);(\d+)M", out)
    if not hits:
        return None
    _b, x, y = hits[-1]
    return int(x) - 1, int(y) - 1


def test_pixel_mode_matches_the_reported_cell():
    with Session(probe(True), cols=70, rows=14) as s:
        s.settle()
        p = s.pane()
        want_x, want_y = 20, 6
        s.click(p["content_x"] + want_x, p["content_y"] + want_y)
        s.settle(60)
        out = s.snapshot().pane_text(p)

        size = reported(out)
        check("the pane got a mode-2048 size report", size is not None, repr(out[:200]))
        got = clicked(out)
        check("and an SGR mouse report", got is not None, repr(out[:200]))
        if not size or not got:
            return
        _cols, _rows, cw, ch = size
        check("the cell size is pixels, not cells", cw > 1 and ch > 1, str(size))
        check(
            "a click divides back down to the cell it hit",
            (got[0] // cw, got[1] // ch) == (want_x, want_y),
            f"report={got} cell={cw}x{ch} -> "
            f"{(got[0] // cw, got[1] // ch)} want={(want_x, want_y)}",
        )


def test_cell_mode_is_unchanged():
    with Session(probe(False), cols=70, rows=14) as s:
        s.settle()
        p = s.pane()
        want_x, want_y = 20, 6
        s.click(p["content_x"] + want_x, p["content_y"] + want_y)
        s.settle(60)
        out = s.snapshot().pane_text(p)
        got = clicked(out)
        check("a plain SGR pane still gets cell coordinates", got == (want_x, want_y), repr(got))


if __name__ == "__main__":
    test_pixel_mode_matches_the_reported_cell()
    test_cell_mode_is_unchanged()
    sys.exit(report())
