#!/usr/bin/env python3
"""scripting -- built to be driven.

Left pane: a script's-eye view of the control socket -- the commands are
typed, the replies printed are byte-for-byte what the socket answers, and
the session performs the verb as each line lands. Right pane: a program
(deploy.py) that drew status text and real buttons into its own frame with
one escape sequence; the pointer clicks [Approve] and the click arrives
back on the program's stdin, which is where it wanted it.
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from castgen import ROOT, Recorder, out_path

ENV = {"PS1": "$ ", "ENV": "/dev/null"}

# Amber, not the house pink: each cast in the tour records under its own
# theme, so scrolling to the next step reads as a scene change.
THEME = os.path.join(ROOT, "contrib", "themes", "amber.kdl")

work = tempfile.mkdtemp(prefix="slosh-demo-")

# The left pane's shell: reads a line, answers like the socket does. The
# replies below are copied from the real api replies at record time.
with open(os.path.join(work, "sh-replay"), "w") as f:
    f.write(
        "printf '$ '\n"
        "while IFS= read -r l; do\n"
        '  case "$l" in\n'
        '    *\'"split"\'*) echo \'{"ok":true,"id":4}\' ;;\n'
        "    *set-name*) echo '{\"ok\":true}' ;;\n"
        "    *) : ;;\n"
        "  esac\n"
        "  printf '$ '\n"
        "done\n"
    )

# The right pane's program: asks for approval in its own frame.
with open(os.path.join(work, "deploy.py"), "w") as f:
    f.write(
        "import os, sys, tty\n"
        "w = sys.stdout.write\n"
        "w('\\033]5577;1;status;release 1.4.2 -- awaiting approval\\033\\\\')\n"
        "w('\\033]5577;1;buttons;approve:Approve;cancel:Cancel\\033\\\\')\n"
        "print('deploy: waiting for a human (or an agent)')\n"
        "sys.stdout.flush()\n"
        "tty.setcbreak(0)\n"
        "buf = ''\n"
        "while 'approve' not in buf:\n"
        "    buf += os.read(0, 64).decode('utf-8', 'replace')\n"
        "w('\\033]5577;1;status;approved\\033\\\\')\n"
        "print('deploy: approved -- rolling out')\n"
        "sys.stdout.flush()\n"
        "os.read(0, 64)\n"
    )

lay = os.path.join(work, "demo.layout")
with open(lay, "w") as f:
    f.write(
        'layout {\n  tab {\n    pane focus=true cwd="%s" command="sh sh-replay"\n'
        '    pane cwd="%s" command="python3 deploy.py"\n  }\n}\n' % (work, work)
    )

r = Recorder(
    ["/bin/sh"],
    cols=100,
    rows=28,
    title="built to be driven",
    config=THEME,
    env=ENV,
    layout=lay,
)

r.capture(force=True, settle_ms=150)
r.pause(1.2)

# One JSON object per line: split, from outside.
r.type('slosh cmd \'{"cmd":"split","dir":"rows"}\'')
r.enter(dt=0.3)
reply = r.api("split", dir="rows")
r.capture(dt=0.4, settle_ms=80)
r.pause(1.4)

# Name the new pane from outside, too: the title changes where it stands.
new_id = reply["id"]
r.type(
    'slosh cmd \'{"cmd":"set-name","target":"pane","id":%d,"name":"build"}\'' % new_id
)
r.enter(dt=0.3)
r.api("set-name", target="pane", id=new_id, name="build")
r.capture(dt=0.4, settle_ms=80)
r.pause(1.4)

# The program in the other pane asked its question in its own frame.
# Answer it with the pointer; the click lands on its stdin.
snap = r.snapshot()
hit = next(e for e in snap.hits if "approve" in e["action"])
r.move_to(hit["x"] + hit["w"] // 2, hit["y"], dur=0.9)
r.pause(0.6)
r.press()
r.release()
r.capture(dt=0.4, settle_ms=150)
r.pause(2.2)

r.save(out_path("scripting"), tail=2.0)
shutil.rmtree(work)
