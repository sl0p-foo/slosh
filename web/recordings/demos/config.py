#!/usr/bin/env python3
"""config -- make it yours, one file, live.

C-a e opens the config in an editor pane. Every edit lands the moment it is
saved: the theme flips from pink to green, `padding` pushes the content off
the frames, `compact true` swaps floating boxes for shared dividers. (The
headless recorder asks for the reload the file-watcher would do live.)
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from castgen import THEME, Recorder, out_path

# shortmess: the rotated editor pane is a column, and vim's file-info
# message wraps there into a hit-enter prompt mid-demo. F drops it.
ENV = {
    "PS1": "$ ",
    "ENV": "/dev/null",
    "EDITOR": 'vim -u NONE -i NONE -c "set shortmess+=atoOF"',
}

work = tempfile.mkdtemp(prefix="slosh-demo-")
cfg = os.path.join(work, "config.kdl")
shutil.copyfile(THEME, cfg)

r = Recorder(["/bin/sh"], cols=100, rows=28, title="make it yours", config=cfg, env=ENV)

# Something on the one pane, so geometry changes have a witness. No neighbour
# beyond that: the editor C-a e opens is the only split, and gets half the
# window to itself.
r.capture(force=True)
r.pause(0.6)
r.run(
    "for i in 1 1 2 2 3 3 4 4 5 5 6 6; do"
    " printf '\\033[4%sm%-40s\\033[0m\\n' $i ' '; done"
)
r.capture(dt=0.4)
r.pause(0.8)


def save_and_reload(hold):
    r.type(":w")
    r.enter(dt=0.3)
    r.api("reload")
    r.capture(dt=0.3, settle_ms=60)
    r.pause(hold)


# C-a e: the config, in a pane of its own.
r.key("e", dt=0.5)
r.until_text("frame_focus", dt=0.6, timeout_ms=5000)
r.pause(1.0)

# C-a Space: a quarter turn. The editor arrived as a strip along the
# bottom; turned, it is a column with the height the file deserves.
r.key(" ", dt=0.5)
r.pause(1.0)

# Pink to green: find the focus colour, change the word, save.
r.type("/ff5fd7")
r.enter(dt=0.4)
r.type("cw5fffaf")
r.send("\x1b", dt=0.3)
r.pause(0.5)
save_and_reload(1.6)

# Room to breathe: padding, appended at the end of the file.
r.send("G", dt=0.2)
r.type("opadding 1 2")
r.send("\x1b", dt=0.3)
save_and_reload(1.6)

# A different school of borders entirely.
r.type("ocompact true")
r.send("\x1b", dt=0.3)
save_and_reload(2.2)

r.save(out_path("config"), tail=2.0)
shutil.rmtree(work)
