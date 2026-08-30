#!/usr/bin/env python3
"""reflow -- layouts that reflow the way you expect.

The arrangement is derived from the window, never stored. Turn it a quarter,
shrink the terminal until boxes become a tidy list, grow it back: the exact
arrangement returns, because there was nothing stored to drift.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from castgen import ROOT, Recorder, out_path

ENV = {"PS1": "$ ", "ENV": "/dev/null"}

# Slate, not the house pink: each cast in the tour records under its own
# theme, so scrolling to the next step reads as a scene change.
THEME = os.path.join(ROOT, "contrib", "themes", "slate.kdl")

r = Recorder(
    ["/bin/sh"],
    cols=100,
    rows=28,
    title="layouts that reflow",
    config=THEME,
    env=ENV,
)

# Three named panes, built over the socket -- the demo is about the layout.
r.api("split", dir="cols")
r.api("split", dir="rows")
for pid, name in zip(
    sorted(p["id"] for p in r.api("panes")["panes"]), ["editor", "server", "logs"]
):
    r.api("set-name", target="pane", id=pid, name=name)
r.capture(force=True)
r.pause(1.0)

r.run("echo this arrangement is derived, not stored", wait="derived")
r.pause(1.2)

# A quarter turn: the same panes, the layout re-derived on the spot.
r.key(" ", dt=0.5)
r.pause(1.3)

# Down. Each stop holds long enough to see the layout re-derive itself.
for size in [(84, 24), (66, 18), (50, 13), (36, 8)]:
    r.resize(*size, dt=0.55)
    r.pause(0.75)
r.pause(1.2)  # the small screen: a list, not clutter

# And back. The exact arrangement returns.
for size in [(50, 13), (66, 18), (84, 24), (100, 28)]:
    r.resize(*size, dt=0.55)
    r.pause(0.6)
r.pause(1.6)

r.save(out_path("reflow"), tail=2.0)
