#!/usr/bin/env python3
"""themes -- a theme is a name, and the picker wears it while you browse.

C-a t lists every theme that can be named -- yours, and the ones slosh ships
-- and moving the selection *applies* it: the session behind the box is the
only honest preview of a palette meant to dress a whole session, and a swatch
in a list would be a small lie about one. Typing narrows the list. C-s writes
the one line into the config that keeps it; Escape puts back what was worn.

Only dark themes are visited, and that is not timidity: cells slosh does not
paint keep the *terminal's* own background, here and in a real session alike,
so a light palette in a dark terminal is a thing to try in your terminal
rather than to film in somebody else's.
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from castgen import ROOT, Recorder, out_path

THEMES = os.path.join(ROOT, "contrib", "themes")
ENV = {"PS1": "$ ", "ENV": "/dev/null"}

work = tempfile.mkdtemp(prefix="slosh-demo-")

# A theme directory beside the config, which is what ~/.config/slosh/themes
# is: the shipped set copied where a person keeps theirs, so the cast carries
# no path off the machine that rendered it -- the config on screen says a
# name and nothing else, which is the whole claim of this demo.
shutil.copytree(THEMES, os.path.join(work, "themes"))

cfg = os.path.join(work, "config.kdl")
with open(cfg, "w") as f:
    f.write('theme_name "sl0p"\ntab_bar_side "left"\ntab_bar_width 20\ntab_gap 1\n')

# Something in the panes, so a repaint has more than frames to land on: the
# sidebar carries names, numbers and statuses, and the tab bar is where a
# theme is most itself.
build = os.path.join(work, "build")
with open(build, "w") as f:
    f.write(
        r"""
printf '\033]5577;1;status;zig build\033\\'
printf '\033]5577;1;busy;1\033\\'
printf '  zig build-lib slosh\n  zig build-exe slosh\n'
sleep 600
"""
    )

lay = os.path.join(work, "demo.layout")
with open(lay, "w") as f:
    f.write(
        "layout {\n"
        '  tab name="slosh" {\n'
        '    pane focus=true cwd="%s"\n'
        '    pane command="sh build" cwd="%s"\n'
        "  }\n"
        '  tab name="notes" {\n'
        '    pane cwd="%s"\n'
        "  }\n"
        "}\n" % (work, work, work)
    )

r = Recorder(
    ["/bin/sh"],
    cols=100,
    rows=28,
    title="themes, by name",
    config=cfg,
    env=ENV,
    layout=lay,
)
# The theme the session starts in is the config's, not this file's: the
# recorder reads default_fg/default_bg off the config it was given, and a
# config that names a theme carries neither. sl0p's are the site's own.
r.fg, r.bg = "#ffffff", "#000000"

r.capture(force=True, settle_ms=200)
r.pause(0.7)
r.run("cat config.kdl", wait="theme_name")
r.pause(1.6)

# C-a t: every theme that can be named, with the one in force marked and the
# one the config asks for saying so.
r.key("t", dt=0.6)
r.pause(1.8)

# Down the list. Each step is worn, not previewed: the frames, the tab bar,
# the statuses and the picker itself are all repainted in the theme under the
# cursor before the next keystroke.
for _ in range(2):
    r.send("\x1b[B", dt=0.25)
    r.pause(1.5)

# Typing narrows it -- and narrowing moves the selection, so the session is
# wearing whatever the query lands on.
r.type("pho", cps=0.12)
r.pause(2.0)

# C-s: the one line that keeps it, written into the config and nothing else.
r.send("\x13", dt=0.4)
r.pause(2.2)
r.run("cat config.kdl", wait="phosphor")
r.pause(1.8)

# And the other half of the bargain: a look you did not want costs nothing.
r.key("t", dt=0.6)
r.pause(1.0)
r.send("\x1b[B", dt=0.25)
r.pause(1.4)
r.send("\x1b[B", dt=0.25)
r.pause(1.4)
r.send("\x1b", dt=0.4)  # escape: back to what was worn
r.pause(2.0)

r.save(out_path("themes"), tail=2.0)
shutil.rmtree(work)
