#!/usr/bin/env python3
"""The themes in contrib/.

Mostly a guard: a theme file that has fallen behind the config silently keeps
the compiled-in default for whatever it forgot, which looks like a bug in the
theme rather than a missing line in it.
"""
import glob
import os
import re
import sys

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']
HERE = os.path.dirname(os.path.abspath(__file__))
THEMES = sorted(glob.glob(os.path.join(HERE, "..", "contrib", "themes", "*.kdl")))


def declared_in_config():
    src = open(os.path.join(HERE, "..", "src", "config.c")).read()
    return set(re.findall(r'\{"([a-z_]+)", &c->', src))


def set_by(path):
    return set(re.findall(r'^    ([a-z_]+) "', open(path).read(), re.M))


def test_there_are_themes():
    check("contrib ships some", len(THEMES) >= 3, str(THEMES))


def test_each_sets_every_colour_the_config_knows():
    want = declared_in_config()
    for t in THEMES:
        missing = want - set_by(t)
        check(f"{os.path.basename(t)} sets all {len(want)} of them",
              not missing, "missing: " + ", ".join(sorted(missing)))


def test_each_one_parses_and_reaches_the_screen():
    for t in THEMES:
        declared = dict(re.findall(r'^    ([a-z_]+) "(#[0-9a-fA-F]{6})"',
                                   open(t).read(), re.M))
        with Session(SH, cols=50, rows=10, config=os.path.abspath(t)) as s:
            s.settle(30)
            snap, p = s.snapshot(), s.pane()
            drawn = (snap.style_at(p["x"], p["y"] + 1) or {}).get("fg")
            check(f"{os.path.basename(t)} draws its own frame colour",
                  drawn == declared["frame_focus"],
                  f"{drawn} != {declared['frame_focus']}")
            check(f"{os.path.basename(t)} leaves the session alive", s.alive(), "")


if __name__ == "__main__":
    test_there_are_themes()
    test_each_sets_every_colour_the_config_knows()
    test_each_one_parses_and_reaches_the_screen()
    sys.exit(report())
