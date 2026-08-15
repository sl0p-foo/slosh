#!/usr/bin/env python3
"""`include "themes/nord.kdl"`: a config made of files.

The thing it is for is switching a theme without pasting one, but nothing about
it is theme-specific — an include is another config file applied here, so the
same line composes keys, shaders, states or geometry.

Two rules carry the weight, and both are checked below: a relative path is
relative to *the file doing the including*, never to the working directory; and
what you include is the base, so the file that includes it wins. The loader
reads a document by asking it for the keys it knows rather than walking it in
order, so "here" is not a position it could honour anyway.
"""
import os
import re
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]


def tree(files, home=None):
    """Write {relative path: text} into a fresh directory; returns its path."""
    root = tempfile.mkdtemp()
    for rel, text in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    return root


def frame_fg(s):
    """The colour of the focused pane's top rule."""
    p = s.pane()
    return (s.snapshot().style_at(p["x"] + 5, p["y"]) or {}).get("fg")


GREEN = 'theme { frame_focus "#00ff00" }\n'
BLUE = 'theme { frame_focus "#0000ff" }\n'


def test_an_included_theme_applies():
    root = tree({"themes/green.kdl": GREEN, "config.kdl": 'include "themes/green.kdl"\n'})
    with Session(SH, cols=60, rows=14, config=root + "/config.kdl") as s:
        s.settle()
        check("the frame wears the included colour", frame_fg(s) == "#00ff00",
              str(frame_fg(s)))


def test_the_file_doing_the_including_wins():
    """So a theme is a base you can put two lines on top of."""
    root = tree({"themes/green.kdl": GREEN,
                 "config.kdl": 'include "themes/green.kdl"\n' + BLUE})
    with Session(SH, cols=60, rows=14, config=root + "/config.kdl") as s:
        s.settle()
        check("the outer file's colour is the one on screen",
              frame_fg(s) == "#0000ff", str(frame_fg(s)))

    # ...and the order of the two lines does not change that, because an include
    # is applied before the file it appears in, wherever it appears.
    root = tree({"themes/green.kdl": GREEN,
                 "config.kdl": BLUE + 'include "themes/green.kdl"\n'})
    with Session(SH, cols=60, rows=14, config=root + "/config.kdl") as s:
        s.settle()
        check("even with the include written last", frame_fg(s) == "#0000ff",
              str(frame_fg(s)))


def test_later_includes_win_over_earlier_ones():
    root = tree({"a.kdl": GREEN, "b.kdl": BLUE,
                 "config.kdl": 'include "a.kdl"\ninclude "b.kdl"\n'})
    with Session(SH, cols=60, rows=14, config=root + "/config.kdl") as s:
        s.settle()
        check("the last one applied is the one that shows",
              frame_fg(s) == "#0000ff", str(frame_fg(s)))


def test_one_include_can_take_several_files():
    root = tree({"a.kdl": GREEN, "b.kdl": 'gap 2\n',
                 "config.kdl": 'include "a.kdl" "b.kdl"\n'})
    with Session(SH, cols=60, rows=14, config=root + "/config.kdl") as s:
        s.settle()
        check("both were applied",
              frame_fg(s) == "#00ff00" and s.pane()["x"] == 4,
              f"{frame_fg(s)} / x={s.pane()['x']}")


def test_a_path_is_relative_to_the_file_that_wrote_it():
    """The nested file says `include "base/core.kdl"`, which only resolves if it
    is read relative to the theme file rather than to the top config."""
    root = tree({"themes/base/core.kdl": 'theme { frame_focus "#ff00ff" }\n',
                 "themes/nested.kdl": 'include "base/core.kdl"\n',
                 "config.kdl": 'include "themes/nested.kdl"\n'})
    with Session(SH, cols=60, rows=20, config=root + "/config.kdl") as s:
        s.settle()
        check("the nested include resolved", frame_fg(s) == "#ff00ff",
              str(frame_fg(s)))


def test_a_leading_tilde_is_a_home_directory():
    home = tree({"mytheme.kdl": 'theme { frame_focus "#abcdef" }\n'})
    root = tree({"config.kdl": 'include "~/mytheme.kdl"\n'})
    with Session(SH, cols=60, rows=14, config=root + "/config.kdl",
                 env={"HOME": home}) as s:
        s.settle()
        check("~ expanded to the home directory", frame_fg(s) == "#abcdef",
              str(frame_fg(s)))


def test_anything_can_be_included_not_only_a_theme():
    root = tree({"keys.kdl": 'keys {\n    bind "v" "split-cols"\n}\n',
                 "config.kdl": 'include "keys.kdl"\n'})
    with Session(SH, cols=90, rows=14, config=root + "/config.kdl") as s:
        s.settle()
        s.key("v")
        s.settle()
        check("a binding from an included file works", len(s.panes()) == 2,
              str(s.panes()))


def test_a_missing_include_costs_a_line_and_no_more():
    """Losing your keybindings over a mistyped theme name would be a worse
    answer than a session that carries on without the theme (D9) -- but silence
    would be its own kind of wrong, so it is a line and not nothing."""
    root = tree({"config.kdl": 'include "themes/nope.kdl"\ngap 2\n'})
    with Session(SH, cols=60, rows=14, config=root + "/config.kdl") as s:
        s.settle()
        check("the session is running", s.alive())
        check("and the rest of the file applied anyway", s.pane()["x"] == 4,
              str(s.pane()))

        reply = s.api("reload")
        check("the reload succeeds", reply.get("ok"), str(reply))
        check("...and says which line it could not honour",
              "nope.kdl" in reply.get("warning", ""), str(reply))
        check("the session says it out loud too",
              "nope.kdl" in s.snapshot().screen(),
              repr(s.snapshot().screen()[-200:]))


def test_a_cycle_says_so():
    root = tree({"a.kdl": 'include "b.kdl"\n', "b.kdl": 'include "a.kdl"\n'})
    with Session(SH, cols=70, rows=14, config=root + "/a.kdl") as s:
        s.settle()
        reply = s.api("reload")
        check("a cycle is reported rather than followed",
              "too deep" in reply.get("warning", ""), str(reply))


def test_a_cycle_stops_rather_than_spinning():
    """A config that includes itself is a mistake, and the depth limit is what
    turns it into a message instead of a session that never starts."""
    root = tree({"a.kdl": 'include "b.kdl"\ntheme { frame_focus "#00ff00" }\n',
                 "b.kdl": 'include "a.kdl"\n'})
    with Session(SH, cols=60, rows=14, config=root + "/a.kdl") as s:
        s.settle()
        check("the session started at all", s.alive())
        check("and the file that included itself still applied",
              frame_fg(s) == "#00ff00", str(frame_fg(s)))


def test_an_include_is_picked_up_by_a_reload():
    root = tree({"themes/t.kdl": GREEN, "config.kdl": 'include "themes/t.kdl"\n'})
    with Session(SH, cols=60, rows=14, config=root + "/config.kdl") as s:
        s.settle()
        check("green to start", frame_fg(s) == "#00ff00", str(frame_fg(s)))

        # Switching theme is editing one line, which is the point of the feature.
        with open(root + "/themes/other.kdl", "w") as f:
            f.write(BLUE)
        with open(root + "/config.kdl", "w") as f:
            f.write('include "themes/other.kdl"\n')
        s.api("reload")
        s.settle(30)
        check("and blue after the include line changed",
              frame_fg(s) == "#0000ff", str(frame_fg(s)))


def test_every_contrib_theme_can_be_included():
    """They are written to be pasted; including one has to work too, or the
    directory is only half a feature. Checked against each file's own
    `frame_focus`, so this cannot pass by the theme happening to look default."""
    here = os.path.dirname(os.path.abspath(__file__))
    tdir = os.path.abspath(os.path.join(here, "..", "contrib", "themes"))
    themes = sorted(f for f in os.listdir(tdir) if f.endswith(".kdl"))
    check("contrib ships themes", len(themes) >= 5, str(themes))

    root = tree({"config.kdl": ""})
    with Session(SH, cols=60, rows=14, config=root + "/config.kdl") as s:
        s.settle()
        for name in themes:
            path = os.path.join(tdir, name)
            want = re.search(r'frame_focus\s+"(#[0-9a-fA-F]{6})"', open(path).read())
            check(f"{name} declares a frame colour", want is not None, name)
            with open(root + "/config.kdl", "w") as f:
                f.write('include "%s"\n' % path)
            s.api("reload")
            s.settle(30)
            check(f"{name} applies as an include",
                  want and frame_fg(s) == want.group(1).lower(),
                  f"{name}: wanted {want and want.group(1)}, got {frame_fg(s)}")


if __name__ == "__main__":
    test_an_included_theme_applies()
    test_the_file_doing_the_including_wins()
    test_later_includes_win_over_earlier_ones()
    test_one_include_can_take_several_files()
    test_a_path_is_relative_to_the_file_that_wrote_it()
    test_a_leading_tilde_is_a_home_directory()
    test_anything_can_be_included_not_only_a_theme()
    test_a_missing_include_costs_a_line_and_no_more()
    test_a_cycle_says_so()
    test_a_cycle_stops_rather_than_spinning()
    test_an_include_is_picked_up_by_a_reload()
    test_every_contrib_theme_can_be_included()
    sys.exit(report())
