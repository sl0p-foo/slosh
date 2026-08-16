#!/usr/bin/env python3
"""Every shader in contrib/shaders/ works in sl0ppty.

The toy proves the *language* matches (test_shadertoy.py). This proves the
shipped files: that each one parses as config, is accepted by the shader
registry, and actually changes the cells of a real pane. An example that
previews beautifully and does nothing when you paste it is worse than no
example, and nothing else would catch that.

One session, reloaded per preset, because that is both fast and a fair
exercise of the reload path -- which is how anyone will actually try these.
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time

from harness import Session, check, report

HERE = os.path.dirname(os.path.abspath(__file__))
CONTRIB = os.path.join(HERE, "..", "contrib")
FILES = sorted(glob.glob(os.path.join(CONTRIB, "shaders", "*.kdl")))

# Prints a screenful of known-coloured text, then waits.
SH = ["/bin/sh", "-c",
      'printf "\\033]2;p\\007"; '
      'i=0; while [ $i -lt 12 ]; do printf "%s\\n" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789; '
      'i=$((i+1)); done; read x']

# Every colour in the pane's content area, so "did anything change" is a
# question about the whole pane rather than one lucky cell.
def cells(snap, pane):
    out = []
    for row in range(pane["content_h"]):
        y = pane["content_y"] + row
        for col in range(pane["content_w"]):
            run = snap.style_at(pane["content_x"] + col, y)
            out.append((run or {}).get("fg"))
    return out


def test_every_preset_changes_the_pane():
    check("contrib ships shader presets", len(FILES) >= 20, str(len(FILES)))

    cfg = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    cfg.write('theme { default_fg "#ffffff" default_bg "#000000" }\n')
    cfg.close()

    with Session(SH, cols=70, rows=18, config=cfg.name) as s:
        s.until_text("ABCDEF")
        base = cells(s.snapshot(), s.pane())

        for path in FILES:
            name = os.path.basename(path)
            # The theme keeps "what colour is a cell" answerable; the preset is
            # appended so it is exactly the file a user would paste.
            with open(cfg.name, "w") as f:
                f.write('theme { default_fg "#ffffff" default_bg "#000000" }\n')
                f.write(open(path).read())
            reply = s.api("reload")
            check(f"{name} loads", reply.get("ok") is True, str(reply))

            # Polled rather than sampled once. An animated preset can be at a
            # phase where it genuinely does nothing -- `motion-typewriter` dims
            # the rows below a line that walks down the pane, and one frame in
            # `rows` that line is past the last row -- and a pass at zero
            # strength now leaves the cells alone instead of quietly rewriting
            # them in the same colours. So: wait for it to show itself.
            now = base
            deadline = time.monotonic() + 2.5
            while now == base and time.monotonic() < deadline:
                s.settle(20)
                now = cells(s.snapshot(), s.pane())
            check(f"{name} changes the pane", now != base,
                  f"identical to unshaded: {now[:3]}")

    os.unlink(cfg.name)


def test_a_few_of_them_do_the_specific_thing_they_claim():
    """Spot checks with a known answer, so "it changed something" cannot be
    satisfied by an effect that changed the wrong thing."""
    cfg = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)

    def load(basename):
        with open(cfg.name, "w") as f:
            f.write('theme { default_fg "#ffffff" default_bg "#000000" }\n')
            f.write(open(os.path.join(CONTRIB, "shaders", basename)).read())

    load("built-ins-scanlines.kdl")
    with Session(SH, cols=70, rows=18, config=cfg.name) as s:
        s.until_text("ABCDEF")
        pane = s.pane()
        rows = [cells(s.snapshot(), pane)[r * pane["content_w"]]
                for r in range(4)]
        check("scanlines alternate row by row",
              rows[0] == rows[2] and rows[1] == rows[3] and rows[0] != rows[1],
              str(rows))

    load("guides-indent-guides.kdl")
    with Session(SH, cols=70, rows=18, config=cfg.name) as s:
        s.until_text("ABCDEF")
        pane = s.pane()
        row = cells(s.snapshot(), pane)[:9]
        check("indent guides mark every fourth column",
              row[0] == row[4] == row[8] and row[0] != row[1] and row[1] == row[2],
              str(row))

    load("pane-states-unfocused.kdl")
    with Session(SH, cols=70, rows=18, config=cfg.name) as s:
        s.until_text("ABCDEF")
        pane = s.pane()
        flat = cells(s.snapshot(), pane)
        check("a flat dim is the same everywhere",
              len({c for c in flat[:40] if c}) == 1, str(flat[:6]))

    os.unlink(cfg.name)


def test_the_tour_can_name_every_one_of_them():
    """contrib/shader-tour resolves a short name (`torch`) as well as the whole
    slug. A tour that cannot name a shader it ships is a broken demo."""
    tour = os.path.join(CONTRIB, "shader-tour")
    conf = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    conf.close()
    env = dict(os.environ, SL0PPTY_CONFIG=conf.name)

    # Named explicitly rather than derived: a group name with a hyphen in it
    # (built-ins) makes "the short half" ambiguous to compute and easy to get
    # accidentally right, which is not a test.
    for name in ("guides-torch", "torch", "matrix-rain"):
        out = subprocess.run([tour, name], capture_output=True, text=True, env=env)
        check(f"the tour applies {name!r}",
              out.returncode == 0 and "shaders {" in open(conf.name).read(),
              (out.stdout + out.stderr).strip())

    out = subprocess.run([tour, "definitely-not-a-shader"],
                         capture_output=True, text=True, env=env)
    check("and refuses one it does not have", out.returncode != 0,
          (out.stdout + out.stderr).strip())
    os.unlink(conf.name)


def test_the_files_match_the_page_they_came_from():
    """They are generated from contrib/shadertoy.html. A hand-edit there and a
    forgotten regeneration would leave the two disagreeing silently."""
    if not shutil.which("node"):
        print("SKIP no node: cannot check the generated files are current")
        return
    out = subprocess.run([os.path.join(CONTRIB, "gen-shaders"), "--check"],
                         capture_output=True, text=True)
    check("contrib/shaders is current with contrib/shadertoy.html",
          out.returncode == 0, (out.stderr or out.stdout).strip())


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
