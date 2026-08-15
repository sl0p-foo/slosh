#!/usr/bin/env python3
"""Every chrome preset in contrib/chrome/ works in sl0ppty.

The content presets are cross-checked against the toy that previews them
(test_shadertoy.py). A frame has no preview — the toy draws a pane's contents —
so these are checked the only way left that means anything: load the file into a
real session and look at the cells. An example that reads beautifully and does
nothing when you paste it is worse than no example.

Two things are asserted of each, and the second matters as much as the first:
the frame moves, and the pane's own output does not. A chrome pass that leaked
into the contents would be a shader bug wearing a preset's clothes.
"""
import glob
import os
import sys
import tempfile
import time

from harness import Session, check, report

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = sorted(glob.glob(os.path.join(HERE, "..", "contrib", "chrome", "*.kdl")))

# Prints a screenful of known-coloured text, then waits.
SH = ["/bin/sh", "-c",
      'printf "\\033]2;p\\007"; '
      'printf "\\033[38;2;0;255;0mBLOCK\\033[0m\\n"; stty raw -echo; cat']

# Announcements stack up from the bottom right and are drawn over everything,
# including a pane's bottom rule -- and this test reloads eight times. A toast
# that outlived its reload would be measured as the frame having changed, so
# they are told to expire immediately.
THEME = ('theme { default_fg "#ffffff" default_bg "#000000" }\n'
         'toast_ms 1\n')


def frame_cells(snap, pane):
    """Every cell of the frame's outer ring, clockwise from the top-left."""
    x0, y0, w, h = pane["x"], pane["y"], pane["w"], pane["h"]
    out = []
    for x in range(w):
        out.append(snap.style_at(x0 + x, y0))
    for y in range(1, h):
        out.append(snap.style_at(x0 + w - 1, y0 + y))
    for x in range(w - 2, -1, -1):
        out.append(snap.style_at(x0 + x, y0 + h - 1))
    for y in range(h - 2, 0, -1):
        out.append(snap.style_at(x0, y0 + y))
    return [(r or {}).get("fg") for r in out]


def content_cells(snap, pane):
    return [(snap.style_at(pane["content_x"] + c, pane["content_y"] + r) or {}).get("fg")
            for r in range(pane["content_h"]) for c in range(pane["content_w"])]


def test_every_chrome_preset_moves_the_frame():
    check("contrib ships chrome presets", len(FILES) >= 5, str(len(FILES)))

    cfg = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    cfg.write(THEME)
    cfg.close()

    with Session(SH, cols=54, rows=12, config=cfg.name) as s:
        s.until_text("BLOCK")
        base_snap = s.snapshot()
        base_frame = frame_cells(base_snap, s.pane())
        base_content = content_cells(base_snap, s.pane())

        for path in FILES:
            name = os.path.basename(path)
            with open(cfg.name, "w") as f:
                f.write(THEME + open(path).read())
            s.api("reload")
            s.settle(40)  # past toast_ms, so no announcement is on the frame

            # Sampled over time, because most of these are animated and a
            # single frame can legitimately catch one between pulses.
            seen = set()
            content_moved = False
            for _ in range(5):
                snap = s.snapshot()
                pane = s.pane()
                seen.update(
                    i for i, (a, b) in enumerate(zip(base_frame, frame_cells(snap, pane)))
                    if a != b)
                if content_cells(snap, pane) != base_content:
                    content_moved = True
                time.sleep(0.05)

            check(f"{name} colours the frame", len(seen) > 0, "no cell changed")
            check(f"{name} leaves the contents alone", not content_moved, "content changed")

        # ...and taking it out again puts the frame back, so one preset cannot
        # leave a mark on the next.
        with open(cfg.name, "w") as f:
            f.write(THEME)
        s.api("reload")
        s.settle(40)
        check("removing the last one restores the frame",
              frame_cells(s.snapshot(), s.pane()) == base_frame, "still shaded")

    os.unlink(cfg.name)


if __name__ == "__main__":
    test_every_chrome_preset_moves_the_frame()
    sys.exit(report())
