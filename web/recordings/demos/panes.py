#!/usr/bin/env python3
"""panes -- panes and tabs, the way your hands expect.

The opener: keys and mouse doing the same work. Split with a chord, split by
clicking a border, drag the gap, then carry a pane by its title -- dropped in
the middle it swaps, dropped on an edge band it tucks in beside, which is the
whole layout rebuilt without the keyboard. Zoom, and hop through a tab, to
close. Every coordinate is read from the compositor's hit list, so the demo
cannot disagree with the program.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from castgen import Recorder, out_path

ENV = {"PS1": "$ ", "ENV": "/dev/null"}

r = Recorder(["/bin/sh"], cols=100, rows=28, title="panes and tabs", env=ENV)

r.capture(force=True)
r.pause(0.8)

# A chord: C-a \ splits into columns.
r.key("\\")
r.pause(0.5)
r.run("echo splits are one chord away", wait="one chord")
r.pause(0.9)

# Or a click: the middle of a border splits toward it, and the hover says so
# before the button goes down.
h = r.snapshot().handle(4, "b") or r.snapshot().handle(2, "b")
r.move_to(*h, dur=0.8)
r.pause(0.8)
r.press()
r.release()
r.pause(0.9)

# Name the players so the next two moves read as something.
ids = sorted(p["id"] for p in r.api("panes")["panes"])
for pid, name in zip(ids, ["editor", "server", "logs"]):
    r.api("set-name", target="pane", id=pid, name=name)
r.capture(dt=0.4)
r.pause(0.6)

# Drag the gap: the boundary follows the pointer.
h = r.snapshot().handle(ids[0], "r")
r.move_to(*h, dur=0.7)
r.press()
r.drag_to(h[0] - 13, h[1], dur=0.8)
r.release()
r.pause(1.0)

# Drag a pane by its title onto another: dropped in the middle, they swap.
snap = r.snapshot()
title = next(e for e in snap.hits if e["action"] == f"title:{ids[0]}")
tx, ty = title["x"] + title["w"] // 2, title["y"]
other = next(p for p in r.api("panes")["panes"] if p["id"] != ids[0])
r.move_to(tx, ty, dur=0.7)
r.press()
r.drag_to(
    other["content_x"] + other["content_w"] // 2,
    other["content_y"] + other["content_h"] // 2,
    dur=0.9,
)
r.pause(0.4)
r.release()
r.pause(1.1)

# The same grip, the other drop: carried over a pane, it subdivides into a
# centre and four edge bands. The centre is the swap above; a band means "in
# beside it, on this side", and fills the half of the pane the drop would
# hand over while every other candidate greys out. Linger over the centre
# first, then steer into the bottom band -- read off the hit list, like
# every promise this program draws. Drop: the layout rebuilds, no keyboard.
# The source is the top-right pane; the target the pane alone in the left
# column, so the drop restacks the whole tab -- a rearrangement no swap
# could produce, which is the point of the bands.
panes = r.api("panes")["panes"]
src = next(p for p in panes if p["id"] == ids[0])
target = min((p for p in panes if p["id"] != ids[0]), key=lambda p: p["content_x"])
title = next(e for e in r.snapshot().hits if e["action"] == f"title:{ids[0]}")
r.move_to(title["x"] + title["w"] // 2, title["y"], dur=0.7)
r.press()
r.drag_to(
    target["content_x"] + target["content_w"] // 2,
    target["content_y"] + target["content_h"] // 2,
    dur=0.8,
)
r.pause(0.7)
band = next(e for e in r.snapshot().hits if e["action"] == f"drop:{target['id']}:b")
r.drag_to(band["x"] + band["w"] // 2, band["y"] + band["h"] // 2, dur=0.5)
r.pause(0.9)
r.release()
r.pause(1.4)

# Zoom the focused pane to the whole tab, and back.
r.key("z")
r.pause(1.0)
r.key("z")
r.pause(0.8)

# A fresh tab, and back: the first one kept everything.
r.key("c")
r.pause(0.6)
r.run("echo a fresh tab costs nothing", wait="costs")
r.pause(0.9)
r.key("1")
r.pause(1.6)

r.save(out_path("panes"), tail=1.8)
