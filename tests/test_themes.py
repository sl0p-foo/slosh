#!/usr/bin/env python3
"""The themes in contrib/.

Mostly a guard: a theme file that has fallen behind the config silently keeps
the compiled-in default for whatever it forgot, which looks like a bug in the
theme rather than a missing line in it.
"""

import glob
import os
import re
import subprocess
import sys
import tempfile

from harness import BIN, Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']
HERE = os.path.dirname(os.path.abspath(__file__))
THEMEDIR = os.path.join(HERE, "..", "contrib", "themes")
THEMES = sorted(glob.glob(os.path.join(THEMEDIR, "*.kdl")))


# Colours a theme is *not* expected to name. Two kinds, and both are the
# point of the feature rather than an oversight: the derived ones are mixed
# from colours the theme does define (so a theme gets them in its own family
# without a line), and the opt-in ones are unset on purpose, because a band
# behind a row takes a translucent terminal's own background away.
DERIVED = {
    "tab_status_fg",
    "tab_status_stripe",
    "tab_status_busy",
    "tab_status_spinner",
    "tab_idle_bg",
}
OPT_IN = {"tab_status_bg", "attach_bg"}
# The scrollback search tints: one theme in contrib names them and the rest
# take the compiled-in pair, which reads the same on every palette here.
SHARED = {"search_fg", "search_bg", "search_cur_fg", "search_cur_bg"}


def declared_in_config():
    """Every colour the loader knows, from the table it walks.

    Matched against `offsetof`, which is how the table is written. It used to
    grep for `&c->` -- the spelling before the table moved to offsets -- and
    matched nothing at all for however many months, so this guard was passing
    by finding no colours to check. A guard that cannot fail is worse than no
    guard, so the count is asserted too.
    """
    src = open(os.path.join(HERE, "..", "src", "config.c")).read()
    return set(re.findall(r'\{"([a-z_]+)", offsetof\(config_t', src))


def set_by(path):
    return set(re.findall(r'^    ([a-z_]+) "', open(path).read(), re.M))


def test_there_are_themes():
    check("contrib ships some", len(THEMES) >= 3, str(THEMES))


def test_the_guard_has_something_to_guard():
    want = declared_in_config()
    check("the colour table was found", len(want) > 40, str(len(want)))


def test_each_sets_every_colour_the_config_knows():
    want = declared_in_config() - DERIVED - OPT_IN - SHARED
    for t in THEMES:
        missing = want - set_by(t)
        check(
            f"{os.path.basename(t)} sets all {len(want)} of them",
            not missing,
            "missing: " + ", ".join(sorted(missing)),
        )


def test_default_is_the_compiled_palette_written_out():
    """`theme_name "default"` and naming no theme at all have to be the same
    session -- that claim is the whole reason the file exists, and since the
    config dump stopped writing colours, --dump-theme is what can check it."""
    path = os.path.join(HERE, "..", "contrib", "themes", "default.kdl")
    theirs = dict(
        re.findall(r'^    ([a-z_]+) "(#[0-9a-fA-F]{6})"', open(path).read(), re.M)
    )
    env = dict(os.environ, SLOSH_CONFIG="/nonexistent/slosh.kdl")
    dumped = subprocess.run(
        [BIN, "--dump-theme"], capture_output=True, text=True, env=env
    ).stdout
    ours = dict(re.findall(r'^    ([a-z_]+)\s+"(#[0-9a-fA-F]{6})"', dumped, re.M))
    check("the dump has a palette in it", len(ours) > 40, str(len(ours)))
    wrong = {k: (v, ours.get(k)) for k, v in theirs.items() if ours.get(k) != v}
    check(
        "default.kdl matches the compiled-in palette",
        not wrong,
        "; ".join(f"{k}: file {a}, code {b}" for k, (a, b) in sorted(wrong.items())),
    )


def test_each_one_parses_and_reaches_the_screen():
    for t in THEMES:
        declared = dict(
            re.findall(r'^    ([a-z_]+) "(#[0-9a-fA-F]{6})"', open(t).read(), re.M)
        )
        with Session(SH, cols=50, rows=10, config=os.path.abspath(t)) as s:
            s.settle(30)
            snap, p = s.snapshot(), s.pane()
            drawn = (snap.style_at(p["x"], p["y"] + 1) or {}).get("fg")
            check(
                f"{os.path.basename(t)} draws its own frame colour",
                drawn == declared["frame_focus"],
                f"{drawn} != {declared['frame_focus']}",
            )
            check(f"{os.path.basename(t)} leaves the session alive", s.alive(), "")


def test_each_one_can_be_worn_by_name():
    """What a person actually types: `theme_name "amber"`, not a path to a file
    in a checkout. Every shipped theme has to answer to its own filename."""
    d = tempfile.mkdtemp()
    cfg = os.path.join(d, "config.kdl")
    for t in THEMES:
        name = os.path.basename(t)[:-4]
        declared = dict(
            re.findall(r'^    ([a-z_]+) "(#[0-9a-fA-F]{6})"', open(t).read(), re.M)
        )
        with open(cfg, "w") as f:
            f.write(
                'theme_dir "%s"\ntheme_name "%s"\n' % (os.path.abspath(THEMEDIR), name)
            )
        with Session(SH, cols=50, rows=10, config=cfg) as s:
            s.settle(30)
            snap, p = s.snapshot(), s.pane()
            drawn = (snap.style_at(p["x"], p["y"] + 1) or {}).get("fg")
            check(
                f"theme_name {name!r} is the theme that gets drawn",
                drawn == declared["frame_focus"],
                f"{drawn} != {declared['frame_focus']}",
            )


if __name__ == "__main__":
    test_there_are_themes()
    test_the_guard_has_something_to_guard()
    test_each_sets_every_colour_the_config_knows()
    test_default_is_the_compiled_palette_written_out()
    test_each_one_parses_and_reaches_the_screen()
    test_each_one_can_be_worn_by_name()
    sys.exit(report())
