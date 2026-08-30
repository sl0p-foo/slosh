#!/usr/bin/env python3
"""panes -- panes and tabs, the way your hands expect.

The opener: keys and mouse doing the same work. Split with a chord, split by
clicking a border, drag the gap, swap two panes by dragging a title, zoom,
and hop through a tab. Every coordinate is read from the compositor's hit
list, so the demo cannot disagree with the program.
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

# Drag a pane by its title onto another: they swap.
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
