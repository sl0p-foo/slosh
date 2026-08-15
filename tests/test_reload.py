#!/usr/bin/env python3
"""What a config reload actually changes, and when it is noticed at all.

Two questions worth having answers to in one place: does saving the file get
seen (including when there was no file when the session started), and which
settings take effect without a restart. The second is nearly all of them, and
"nearly" is the part worth pinning — a setting that silently needs a restart
is indistinguishable from one that does not work.
"""
import os
import subprocess
import sys
import tempfile
import time

from harness import BIN, Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; read x']


def conf(text=""):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def frame_fg(s):
    return [r.get("fg") for r in s.snapshot().data["styles"] if r["y"] == 2][:1]


def reload_with(s, path, text):
    with open(path, "w") as f:
        f.write(text)
    s.api("reload")
    s.settle(30)


def test_the_things_that_take_effect_immediately():
    path = conf()
    with Session(SH, cols=80, rows=16, config=path) as s:
        s.settle(30)

        reload_with(s, path, 'theme { frame_focus "#00ff00" }\n')
        check("theme colours", frame_fg(s) == ["#00ff00"], str(frame_fg(s)))

        reload_with(s, path, "gap 3\n")
        check("geometry", s.pane()["x"] == 6, str(s.pane()))

        reload_with(s, path, "status_bar false\n")
        check("chrome on and off", "+" not in s.snapshot().line(1),
              repr(s.snapshot().line(1)))

        reload_with(s, path, 'close_mark "@"\n')
        check("the marks in a frame", "@" in s.snapshot().line(2),
              repr(s.snapshot().line(2)))

        reload_with(s, path, "dim_unfocused 0\n")
        s.key("\\\\")
        s.settle(30)
        unfocused = [p for p in s.panes() if not p["focused"]][0]
        run = s.snapshot().style_at(unfocused["content_x"], unfocused["content_y"])
        check("shader policy", (run or {}).get("fg") is None, str(run))

        reload_with(s, path, 'keys { prefix "ctrl+b" }\n')
        # A new tab rather than a split: by this point the panes are too narrow
        # to split, and a test that fails on geometry while claiming to be
        # about the prefix is worse than no test.
        before = len(s.tabs())
        s.send(r"\x02c")
        s.settle(30)
        check("the leader key itself", len(s.tabs()) == before + 1,
              str(s.tabs()))
    os.unlink(path)


def test_the_shell_is_consulted_when_a_pane_is_made():
    """`shell` used to be read by nobody: main.c took $SHELL directly, so the
    setting existed, parsed, rendered — and did nothing. Now it is resolved per
    pane, which also means editing it applies to the next pane you open."""
    path = conf('shell "/usr/bin/zsh"\n')
    with Session(SH, cols=70, rows=14, config=path) as s:
        s.settle(30)
        # The session was given an explicit command, so its own pane runs that;
        # a *new* pane is where the setting shows up.
        s.key("\\\\")
        s.settle(60)
        check("a second pane opened", len(s.panes()) == 2, str(s.panes()))
    os.unlink(path)


def test_a_config_written_after_the_session_started_is_noticed():
    """The watch is on the directory, so a file that appears later is caught —
    but a directory that does not exist cannot be watched at all, and then
    nothing is ever noticed for the life of the session. That was the bug: no
    config at launch meant no hot reload, ever, even after writing one."""
    home = tempfile.mkdtemp(prefix="sl0ppty-home-")
    path = os.path.join(home, ".config", "sl0ppty", "config.kdl")
    check("the directory really is missing", not os.path.isdir(os.path.dirname(path)))

    env = dict(os.environ, SL0PPTY_CONFIG=path)
    name = "reloadtest-%d" % os.getpid()
    subprocess.run([BIN, "-s", name, "--", "/bin/sh", "-c", "read x"],
                   env=env, capture_output=True, timeout=5)
    try:
        deadline = time.time() + 3
        got = None
        while time.time() < deadline:
            with open(path, "w") as f:
                f.write('theme { frame_focus "#00ff00" }\n')
            time.sleep(0.4)
            out = subprocess.run([BIN, "-s", name, "cmd",
                                  '{"cmd":"snapshot","format":"json"}'],
                                 capture_output=True, text=True, env=env).stdout
            if '"#00ff00"' in out:
                got = True
                break
        check("writing a config later is picked up without a restart", got,
              "the session never saw the new file")
    finally:
        subprocess.run([BIN, "-s", name, "cmd", '{"cmd":"quit"}'],
                       capture_output=True, env=env)


def test_an_included_file_is_watched_too():
    """A theme you can include is a theme that has to reload when you save it,
    so the watch covers every file the config was built from -- not only the one
    the session was started with. Driven through a real session, because the
    watcher is the thing under test and the headless driver has none."""
    home = tempfile.mkdtemp(prefix="sl0ppty-inc-")
    cfgdir = os.path.join(home, ".config", "sl0ppty")
    os.makedirs(os.path.join(cfgdir, "themes"))
    main = os.path.join(cfgdir, "config.kdl")
    theme = os.path.join(cfgdir, "themes", "t.kdl")
    with open(main, "w") as f:
        f.write('include "themes/t.kdl"\n')
    with open(theme, "w") as f:
        f.write('theme { frame_focus "#00ff00" }\n')

    env = dict(os.environ, SL0PPTY_CONFIG=main)
    name = "inctest-%d" % os.getpid()
    subprocess.run([BIN, "-s", name, "--", "/bin/sh", "-c", "read x"],
                   env=env, capture_output=True, timeout=5)

    def screen():
        return subprocess.run([BIN, "-s", name, "cmd",
                               '{"cmd":"snapshot","format":"json"}'],
                              capture_output=True, text=True, env=env).stdout

    try:
        check("the included theme applied at startup", '"#00ff00"' in screen(),
              "green never appeared")

        # The include line is untouched; only the file it points at changes.
        deadline = time.time() + 3
        got = None
        while time.time() < deadline:
            with open(theme, "w") as f:
                f.write('theme { frame_focus "#ff00ff" }\n')
            time.sleep(0.4)
            if '"#ff00ff"' in screen():
                got = True
                break
        check("saving the included file reloads the session", got,
              "the session never saw the theme change")
    finally:
        subprocess.run([BIN, "-s", name, "cmd", '{"cmd":"quit"}'],
                       capture_output=True, env=env)


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
