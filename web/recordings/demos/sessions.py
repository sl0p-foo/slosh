#!/usr/bin/env python3
"""sessions -- sessions outlive everything.

Two real sessions run through the whole recording; only the bare-shell
frames between them are authored (the writer owns the byte stream). Attach
to `main`, work, detach. Attach to `scratch` -- a different theme, so there
is no mistaking them. Come back to `main`: everything exactly as left,
including the pane whose command died while nobody was watching -- output,
exit status, and [re-run] in the frame.
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from castgen import ROOT, Recorder, out_path
from harness import Session

ENV = {"PS1": "$ ", "ENV": "/dev/null"}

work = tempfile.mkdtemp(prefix="slosh-demo-")
# main's logs pane: alive until nudged, then it dies with its story told.
with open(os.path.join(work, "serve"), "w") as f:
    f.write(
        "echo listening on :8080\nread x\necho error: connection reset by peer\nexit 3\n"
    )
lay_main = os.path.join(work, "main.layout")
with open(lay_main, "w") as f:
    f.write(
        "layout {\n  tab {\n    pane focus=true\n    pane\n"
        '    pane cwd="%s" command="sh serve"\n  }\n}\n' % work
    )
lay_scratch = os.path.join(work, "scratch.layout")
with open(lay_scratch, "w") as f:
    f.write("layout {\n  tab {\n    pane focus=true\n    pane\n  }\n}\n")

# Session A: `main`, the sl0p pink. Session B: `scratch`, phosphor green --
# distinct at a glance, which is the point of showing two.
a = Session(
    ["/bin/sh"],
    cols=100,
    rows=28,
    env=ENV,
    layout=lay_main,
    config=os.path.join(ROOT, "contrib", "themes", "sl0p.kdl"),
)
b = Session(
    ["/bin/sh"],
    cols=100,
    rows=28,
    env=ENV,
    layout=lay_scratch,
    config=os.path.join(ROOT, "contrib", "themes", "phosphor.kdl"),
)

r = Recorder(
    ["/bin/sh"], cols=100, rows=28, title="sessions outlive everything", env=ENV
)
r.s.close()  # the recorder's own session is unused: it records a and b

# -- a bare shell, typing its way in ------------------------------------
sh = ["$ "]
r.shell(sh, dt=0.0)
r.hold(0.7)
r.shell_type(sh, "slosh -s main")
r.hold(0.4)

# -- session main: work happens ------------------------------------------
r.switch(a, settle_ms=150)
ids = sorted(p["id"] for p in a.api("panes")["panes"])
for pid, name in zip(ids, ["editor", "server", "logs"]):
    a.api("set-name", target="pane", id=pid, name=name)
r.capture(dt=0.3)
r.pause(0.8)
r.run("echo the deploy notes, half-written", wait="half-written")
r.pause(1.4)

# -- detach ---------------------------------------------------------------
sh = ["$ slosh -s main", "[detached]", "$ "]
r.shell(sh, dt=0.3)
r.hold(0.9)
r.shell_type(sh, "slosh -s scratch")
r.hold(0.4)

# -- session scratch: another life, another look --------------------------
r.switch(b, settle_ms=150)
r.pause(0.8)
r.run("echo a different session, a different colour", wait="different")
r.pause(1.4)

# While nobody is looking at main, its logs pane dies. (The session lives
# on off camera; this is the api nudging its fuse, not a trick.)
logs = ids[2]
a.api("focus", id=logs)
a.api("send", data="\n")
a.settle(150)
a.api("focus", id=ids[0])
a.settle(50)

# -- detach, come home -----------------------------------------------------
sh = ["$ slosh -s scratch", "[detached]", "$ "]
r.shell(sh, dt=0.3)
r.pause(0.8)
r.shell_type(sh, "slosh -s main")
r.hold(0.4)

# -- main again: exactly as left, plus one epitaph -------------------------
r.switch(a, settle_ms=150)
r.pause(1.6)

# The pane that died while we were away kept its output and its exit
# status, and offers the only two verbs left. Run it again.
snap = r.snapshot()
hit = next(e for e in snap.hits if e["action"] == f"rerun:{logs}")
r.move_to(hit["x"] + hit["w"] // 2, hit["y"], dur=0.9)
r.pause(0.7)
r.press()
r.release()
r.capture(dt=0.3, settle_ms=150)
r.pause(1.8)

r.save(out_path("sessions"), tail=2.0)
b.close()
a.close()
shutil.rmtree(work)
