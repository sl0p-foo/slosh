#!/usr/bin/env python3
"""M6: the config file (D2).

Everything here was a compiled-in constant until now. The important property
is not that settings apply — it is that a broken config costs a warning and
never a terminal.
"""
import os
import subprocess
import sys
import tempfile

from harness import Session, check, report, BIN

SH = ["/bin/sh", "-c", 'printf "\\033]2;cfg\\007"; stty raw -echo; cat']


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def test_geometry():
    path = cfg("""
        gap 2
        gap_aspect 1
        rounded false
        title_align "left"
    """)
    with Session(SH, cols=60, rows=14, config=path) as s:
        s.settle()
        snap, p = s.snapshot(), s.pane()
        check("gap is applied", p["x"] == 2 and p["y"] == 3, str(p))
        row = snap.line(p["y"])
        check("rounded false gives square corners",
              "┌" in row and "╭" not in row, repr(row))
        check("title_align left puts the title at the left",
              row.index("cfg") < len(row) // 3, repr(row))
    os.unlink(path)


def test_status_bar_off():
    path = cfg("status_bar false\n")
    with Session(SH, cols=50, rows=10, config=path) as s:
        s.settle()
        snap, p = s.snapshot(), s.pane()
        check("no status bar means no strip row", "panes" not in snap.screen(),
              repr(snap.line(1)))
        check("and the panes get the row back", p["y"] == 1, str(p))
    os.unlink(path)


def test_theme():
    path = cfg("""
        theme {
            frame_focus "#00ff00"
            frame_idle "#333333"
        }
    """)
    with Session(SH, cols=76, rows=14, config=path) as s:
        s.settle()
        p = s.pane()
        run = s.snapshot().style_at(p["x"], p["y"])
        check("the focused frame takes the configured colour",
              run and run["fg"] == "#00ff00", str(run))

        s.key("\\\\")
        s.settle()
        panes = s.panes()
        snap = s.snapshot()
        idle = [q for q in panes if not q["focused"]][0]
        run = snap.style_at(idle["x"], idle["y"])
        check("an unfocused frame takes the other one",
              run and run["fg"] == "#333333", str(run))
    os.unlink(path)


def test_keys():
    path = cfg("""
        keys {
            prefix "ctrl+b"
            bind "v" "split-cols"
            bind "\\\\" "none"
        }
    """)
    with Session(SH, cols=80, rows=16, config=path) as s:
        s.settle()
        s.send(r"\x02v")  # C-b v
        s.settle()
        check("a custom prefix and binding work", len(s.panes()) == 2,
              str(len(s.panes())))

        s.send("\\x02\\\\")  # C-b \  -> unbound now
        s.settle()
        check("a binding set to none does nothing", len(s.panes()) == 2,
              str(len(s.panes())))

        s.send(r"\x01")  # the old prefix is just a keystroke now
        s.settle()
        check("the old prefix is passed through to the pane",
              len(s.panes()) == 2 and s.api("alive")["alive"])

        s.send(r"\x02\x02")  # prefix twice is still a literal prefix
        s.settle()
        check("prefix twice sends the prefix itself", s.api("alive")["alive"])
    os.unlink(path)


def test_min_pane_drives_collapse():
    # Split over the control API rather than the keyboard. This is about what
    # the *layout* does with a split that does not fit, and the keyboard now
    # declines to make one — which is a different rule, tested elsewhere. A
    # script asking for a pane is declaring what it wants, so it still gets it.
    roomy = cfg("min_pane cols=10 rows=3\n")
    tight = cfg("min_pane cols=40 rows=12\n")
    with Session(SH, cols=70, rows=16, config=roomy) as s:
        s.settle()
        s.api("split", dir="cols")
        s.settle()
        check("a low floor keeps both panes visible",
              not any(p["hidden"] for p in s.panes()), str(s.panes()))
    with Session(SH, cols=70, rows=16, config=tight) as s:
        s.settle()
        s.api("split", dir="cols")
        s.settle()
        check("a high floor collapses the same split",
              any(p["hidden"] for p in s.panes()), str(s.panes()))
    os.unlink(roomy)
    os.unlink(tight)


def test_fail_open():
    """A broken config must cost a warning, never a terminal."""
    broken = cfg('gap 1\ntheme {\n  frame_focus "#00ff00"\n')  # unclosed block
    with Session(SH, cols=60, rows=14, config=broken) as s:
        s.settle()
        check("a session still starts on a broken config",
              s.api("alive")["alive"], "")
        p = s.pane()
        check("and uses the compiled-in defaults", p["x"] == 2 and p["y"] == 2,
              str(p))
    proc = subprocess.run([BIN, "--headless", "--cols", "20", "--rows", "3",
                           "--", "/bin/echo", "hi"],
                          capture_output=True, text=True,
                          env=dict(os.environ, SL0PPTY_CONFIG=broken))
    check("the reason is reported on stderr",
          "line" in proc.stderr or "parse" in proc.stderr, repr(proc.stderr))
    check("but the run succeeds", proc.returncode == 0, proc.stderr)
    os.unlink(broken)

    partial = cfg("""
        gap 3
        keys { bind "v" "no-such-action" }
    """)
    with Session(SH, cols=60, rows=14, config=partial) as s:
        s.settle()
        # gap 3, aspect 2 (default) -> 6 columns of margin
        check("a bad binding does not discard the rest of the file",
              s.pane()["x"] == 6, str(s.pane()))
    os.unlink(partial)

    missing = "/nonexistent/definitely/not/here.kdl"
    proc = subprocess.run([BIN, "--headless", "--cols", "20", "--rows", "3",
                           "--", "/bin/echo", "hi"],
                          capture_output=True, text=True,
                          env=dict(os.environ, SL0PPTY_CONFIG=missing))
    check("a missing config is silent", proc.stderr == "", repr(proc.stderr))


def test_reload():
    path = cfg("gap 1\n")
    with Session(SH, cols=60, rows=14, config=path) as s:
        s.settle()
        check("before reload", s.pane()["x"] == 2, str(s.pane()))
        with open(path, "w") as f:
            f.write("gap 3\n")
        r = s.api("reload")
        s.settle()
        check("reload applies the new file", r["ok"] and s.pane()["x"] == 6,
              f"{r} {s.pane()}")

        with open(path, "w") as f:
            f.write("gap 5\ntheme {\n")  # broken
        r = s.api("reload")
        s.settle()
        check("a broken reload is refused", not r["ok"], str(r))
        check("and the working config is kept", s.pane()["x"] == 6,
              str(s.pane()))
    os.unlink(path)


if __name__ == "__main__":
    test_geometry()
    test_status_bar_off()
    test_theme()
    test_keys()
    test_min_pane_drives_collapse()
    test_fail_open()
    test_reload()
    sys.exit(report())
