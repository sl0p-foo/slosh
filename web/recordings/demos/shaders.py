#!/usr/bin/env python3
"""shaders -- colour passes over cells, with strengths written as expressions.

The left pane runs contrib/shader-repl -- the prompt from the docs -- and
what gets typed at it is exactly what a config file would say: a tint over
the frame, a spotlight whose strength is a distance from the cursor (so it
follows the typing), a ruler, scanlines. Then a shipped preset (`:load
shine`) animates a sheen along the frame -- sampled on the session's own
deadline clock -- and, calm restored, the neighbouring pane rings while
unfocused: the border flash is the same kind of pass, over the frame.
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from castgen import ROOT, Recorder, out_path

ENV = {"PS1": "$ ", "ENV": "/dev/null"}

# Mono, not the house pink: this demo is about colour passes, so the base
# theme is the one without colour -- every hue on screen is a shader's. It
# also gives this cast its own look in the tour's theme rotation.
THEME = os.path.join(ROOT, "contrib", "themes", "mono.kdl")

work = tempfile.mkdtemp(prefix="slosh-demo-")
cfg = os.path.join(work, "config.kdl")
with open(cfg, "w") as f:
    f.write(open(THEME).read())
    f.write("\nin_band_shaders true\n")
# The bell, on a fuse: rings one real second after the nudge, so the focus is
# long gone by the time it goes off.
with open(os.path.join(work, "ring"), "w") as f:
    f.write("read x\nsleep 1\nprintf '\\a'\nread y\n")
repl = os.path.join(ROOT, "contrib", "shader-repl")
lay = os.path.join(work, "demo.layout")
with open(lay, "w") as f:
    f.write(
        'layout {\n  tab {\n    pane focus=true command="python3 %s"\n'
        '    pane cwd="%s" command="sh ring"\n  }\n}\n' % (repl, work)
    )

r = Recorder(
    ["/bin/sh"],
    cols=100,
    rows=28,
    title="colour passes over cells",
    config=cfg,
    env=ENV,
    layout=lay,
)

r.capture(force=True, settle_ms=150)
r.pause(1.2)

# A pass over the frame: the chrome> prompt is aimed at it already.
r.run('tint color="#5fffaf" amount=140')
r.pause(1.8)

# Content: a spotlight, strength by distance from the cursor.
r.run(":content")
r.pause(0.6)
r.run('tint amount="255 - dist(x, y, curx, cury) * 14" color="#ff5fd7"')
r.pause(1.0)

# ...which is why it follows the next thing typed: a ruler.
r.run('ruler amount=120 at=50 color="#7aa2f7"')
r.pause(1.8)

# Scanlines: one built-in, one property.
r.run("zebra amount=110 band=1")
r.pause(1.8)

# A shipped preset, and an animated one: a sheen runs the frame. The pass
# reads the clock, so the session asks for frames and pause() samples them.
r.run(":chrome")
r.pause(0.5)
r.run(":load shine")
r.pause(2.4)

# Calm again before the finale; the sheen would talk over the bell.
r.run('tint color="#5fffaf" amount=140')
r.pause(1.0)

# Nudge the other pane's fuse, get the focus away, and let it ring.
panes = r.api("panes")["panes"]
other = next(p for p in panes if not p.get("focused"))
prev = next(p for p in panes if p.get("focused"))
r.api("focus", id=other["id"])
r.capture(dt=0.4)
r.send("\n", dt=0.2)
r.api("focus", id=prev["id"])
r.capture(dt=0.3)
r.wall(1.6)  # real time: the fuse burns, the bell rings unfocused
r.pause(3.0)  # the flash animates on the session's own deadline clock

r.save(out_path("shaders"), tail=2.0)
shutil.rmtree(work)
