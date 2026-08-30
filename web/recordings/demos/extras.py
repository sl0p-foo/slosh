#!/usr/bin/env python3
"""extras -- floats, and the cheatsheet.

C-a F drops a throwaway shell on top of the layout; a monitor runs in it
(a deterministic one -- real htop would record differently every time,
and a recording that cannot be reproduced is a bug here). The float moves
with the keys that move boundaries, grows about its centre, and casts a
shadow, which is how it never passes for a tile. Then C-a ?: the
cheatsheet, built from the bindings actually in force. `exit` closes the
float like any shell, and the layout never flinched.
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from castgen import ROOT, Recorder, out_path

ENV = {"PS1": "$ ", "ENV": "/dev/null"}

# Phosphor: the tour records each cast under its own theme, so scrolling to
# the next step reads as a scene change.
THEME = os.path.join(ROOT, "contrib", "themes", "phosphor.kdl")

work = tempfile.mkdtemp(prefix="slosh-demo-")

# The monitor the float runs: an htop impression with authored numbers.
with open(os.path.join(work, "mon"), "w") as f:
    f.write(
        r"""
G="\033[32m"; Y="\033[33m"; D="\033[2m"; R="\033[7m"; N="\033[0m"
printf "$R mon %-38s$N\n" "up 4:20  load 0.42 0.38 0.35"
printf " 1 [$G|||||||||||||$N%10s45%%]\n" ""
printf " 2 [$G||||||||$N%15s27%%]\n" ""
printf " 3 [$G||||||||||||||||||$Y||$N%3s67%%]\n" ""
printf " 4 [$G|||||$N%18s16%%]\n" ""
printf " mem[$G||||||||||||$N%6s3.1G/7.8G]\n" ""
printf " tasks: ${G}87${N}, ${G}2${N} running\n"
printf "\n"
printf "$D   PID USER   CPU%%  MEM%%  CMD$N\n"
printf " 31337 sl0p   12.3   1.2  slosh\n"
printf "   420 sl0p    9.9   0.8  zig build\n"
printf "  1998 sl0p    2.5   0.4  vim config.kdl\n"
read _
"""
    )

lay = os.path.join(work, "demo.layout")
with open(lay, "w") as f:
    f.write(
        'layout {\n  tab {\n    pane focus=true cwd="%s"\n'
        '    pane cwd="%s"\n  }\n}\n' % (work, work)
    )

r = Recorder(
    ["/bin/sh"],
    cols=100,
    rows=28,
    title="floats, and the cheatsheet",
    config=THEME,
    env=ENV,
    layout=lay,
)

r.capture(force=True, settle_ms=150)
r.pause(0.8)

# Something under way, so the float has something to float over.
r.run("echo the layout, mid-thought", wait="mid-thought")
r.pause(0.9)

# C-a F: a throwaway shell, floating above it all.
r.key("F", dt=0.6)
r.pause(0.9)
r.run("sh mon", wait="31337")
r.pause(1.4)

# It moves with the keys that move boundaries -- a float has no boundary,
# so they move the float -- and grows about its own centre.
for chord in ("L", "L", "K"):
    r.key(chord, dt=0.3)
r.pause(0.7)
r.key("=", dt=0.4)
r.pause(1.2)

# C-a ?: the cheatsheet, built from the bindings actually in force.
r.key("?", dt=0.6)
r.pause(2.4)
r.send("\x1b", dt=0.4)
r.pause(0.8)

# A throwaway shell goes the way of all shells; the layout never flinched.
r.enter(dt=0.2)  # the monitor's read
r.run("exit")
r.pause(1.6)

r.save(out_path("extras"), tail=2.0)
shutil.rmtree(work)
