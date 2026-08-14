#!/usr/bin/env python3
"""Shaders loaded from shared libraries at runtime.

The built-ins are compiled in; these are not. What matters is that a plugin's
shader is indistinguishable from a built-in once loaded (same config syntax,
same parameters, same pass), and that everything which can be wrong with a
library fails safely: a wrong ABI, a name that already exists, a directory
that is not there.

The example in contrib/ is built and loaded here too, so it cannot rot.
"""
import os
import shutil
import sys
import tempfile

from harness import Session, check, report

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", "build")
GOOD = os.path.join(BUILD, "testshader.so")
BAD = os.path.join(BUILD, "badshader.so")
EXAMPLE = os.path.join(BUILD, "exampleshader.so")

# A pane that paints one known cell and then sits still.
SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; printf "BLOCK\\n"; read x']


def session_dir(plugins, config, shaders_subdir=False):
    """A config file, and the plugins it should find beside it."""
    d = tempfile.mkdtemp(prefix="sl0ppty-plug-")
    dest = os.path.join(d, "shaders") if shaders_subdir else d
    os.makedirs(dest, exist_ok=True)
    for p in plugins:
        shutil.copy(p, dest)
    path = os.path.join(d, "config.kdl")
    with open(path, "w") as f:
        f.write(config)
    return path, dest


def bg_at(snap, pane, row=0, col=0):
    run = snap.style_at(pane["content_x"] + col, pane["content_y"] + row)
    return (run or {}).get("bg")


def fg_at(snap, pane, row=0, col=0):
    run = snap.style_at(pane["content_x"] + col, pane["content_y"] + row)
    return (run or {}).get("fg")


def test_a_plugin_shader_is_used_like_a_builtin():
    cfg, d = session_dir([GOOD], 'shader_dir "%s"\nshaders {\n'
                                 '    testfill amount=255 color="#00ff00"\n}\n')
    # the directory is known only after it exists, so write the config now
    with open(cfg, "w") as f:
        f.write('shader_dir "%s"\nshaders {\n'
                '    testfill amount=255 color="#00ff00"\n}\n' % d)

    with Session(SH, cols=40, rows=10, config=cfg) as s:
        snap = s.until_text("BLOCK")
        pane = s.pane()
        check("the loaded shader ran", bg_at(snap, pane) == "#00ff00",
              str(snap.style_at(pane["content_x"], pane["content_y"])))


def test_it_is_found_beside_the_config_by_default():
    cfg, d = session_dir([GOOD], "", shaders_subdir=True)
    with open(cfg, "w") as f:
        f.write('shaders {\n    testfill amount=255 color="#0000ff"\n}\n')

    with Session(SH, cols=40, rows=10, config=cfg) as s:
        snap = s.until_text("BLOCK")
        check("no shader_dir needed", bg_at(snap, s.pane()) == "#0000ff",
              snap.screen())


def test_the_config_number_reaches_the_plugin():
    cfg, d = session_dir([GOOD], "")
    with open(cfg, "w") as f:
        f.write('shader_dir "%s"\nshaders {\n'
                '    teststripe amount=255 band=3 color="#ff0000"\n}\n' % d)

    with Session(SH, cols=40, rows=10, config=cfg) as s:
        snap = s.until_text("BLOCK")
        pane = s.pane()
        got = [bg_at(snap, pane, 0, x) for x in range(6)]
        check("every third column, as asked",
              got[0] == "#ff0000" and got[3] == "#ff0000" and
              got[1] != "#ff0000" and got[2] != "#ff0000", str(got))


def test_a_stale_plugin_is_refused_and_the_rest_still_load():
    cfg, d = session_dir([GOOD, BAD], "")
    with open(cfg, "w") as f:
        f.write('shader_dir "%s"\nshaders {\n'
                '    testfill amount=255 color="#00ff00"\n}\n' % d)

    with Session(SH, cols=40, rows=10, config=cfg) as s:
        snap = s.until_text("BLOCK")
        check("the session is alive", s.alive())
        check("the good plugin still loaded",
              bg_at(snap, s.pane()) == "#00ff00", snap.screen())


def test_an_unknown_shader_is_skipped_not_fatal():
    cfg, d = session_dir([], "")
    with open(cfg, "w") as f:
        f.write('shaders {\n    nosuchshader amount=255\n    dim amount=0\n}\n')

    with Session(SH, cols=40, rows=10, config=cfg) as s:
        snap = s.until_text("BLOCK")
        check("the session survives a name it does not know", s.alive())
        check("and still draws", "BLOCK" in snap.screen(), snap.screen())


def test_no_shader_directory_at_all_is_normal():
    cfg, d = session_dir([], "")
    with open(cfg, "w") as f:
        f.write('shader_dir "/nonexistent/sl0ppty/shaders"\n'
                'shaders {\n    dim amount=100\n}\n')

    with Session(SH, cols=40, rows=10, config=cfg) as s:
        snap = s.until_text("BLOCK")
        check("a missing directory is not an error", s.alive())
        check("and the built-ins are unaffected", "BLOCK" in snap.screen())


def test_the_directory_may_be_written_with_a_tilde():
    cfg, d = session_dir([GOOD], "", shaders_subdir=True)
    home = os.path.dirname(d)
    with open(cfg, "w") as f:
        f.write('shader_dir "~/shaders"\nshaders {\n'
                '    testfill amount=255 color="#00ff00"\n}\n')

    with Session(SH, cols=40, rows=10, config=cfg, env={"HOME": home}) as s:
        snap = s.until_text("BLOCK")
        check("~ in shader_dir is the home directory",
              bg_at(snap, s.pane()) == "#00ff00", snap.screen())


def test_the_contrib_example_builds_and_loads():
    cfg, d = session_dir([EXAMPLE], "")
    with open(cfg, "w") as f:
        f.write('shader_dir "%s"\nshaders {\n'
                '    checker amount=255 band=2\n}\n' % d)

    with Session(SH, cols=40, rows=10, config=cfg) as s:
        snap = s.until_text("BLOCK")
        pane = s.pane()
        # Asserted on the foreground: the checkerboard darkens towards black
        # and the default background already *is* black, so two squares that
        # differ would still read the same there. Reading the wrong channel
        # would make a working effect look like a broken one.
        a = fg_at(snap, pane, 0, 0)
        b = fg_at(snap, pane, 0, 2)
        check("the shipped example draws something", a != b, f"{a} {b}")


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
