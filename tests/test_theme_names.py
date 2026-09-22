#!/usr/bin/env python3
"""Named themes: theme_dir, theme_name, and switching one while it runs.

A theme is a file in a directory and a config names it. The rules being
checked here are the ones that make a *name* worth more than a pasted palette:
the named theme is the base and anything beside it wins, the directory is
searched before the installed one so a theme of yours shadows a shipped one,
a name is a name and not a path, and a switch at runtime survives the reload
that follows it.
"""

import json
import os
import subprocess
import sys
import tempfile

from harness import BIN, Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]

GREEN, PINK, BLUE = "#00ff00", "#ff0088", "#3355ff"


def home(**themes):
    """A config directory: config.kdl plus themes/, and the config's path."""
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, "themes"))
    for name, body in themes.items():
        with open(os.path.join(d, "themes", name + ".kdl"), "w") as f:
            f.write(body)
    return d


def write(path, text):
    with open(path, "w") as f:
        f.write(text)
    return path


def frame(s):
    """The focused pane's frame colour: one cell that every theme names."""
    p = s.pane()
    return (s.snapshot().style_at(p["x"], p["y"] + 1) or {}).get("fg")


def test_a_named_theme_is_read_from_the_theme_dir():
    d = home(lime='theme { frame_focus "%s" }\n' % GREEN)
    cfg = write(os.path.join(d, "config.kdl"), 'theme_name "lime"\n')
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        check("the theme's colour is the one drawn", frame(s) == GREEN, frame(s))


def test_the_config_beats_the_theme_it_named():
    """The same bargain include makes: what you asked for is the base, what you
    wrote beside it is yours -- wherever the line happens to sit."""
    d = home(lime='theme { frame_focus "%s" title "#111111" }\n' % GREEN)
    cfg = write(
        os.path.join(d, "config.kdl"),
        'theme { frame_focus "%s" }\ntheme_name "lime"\n' % PINK,
    )
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        check("the config's own colour wins", frame(s) == PINK, frame(s))


def test_a_theme_dir_of_your_own():
    d = home()
    other = tempfile.mkdtemp()
    write(os.path.join(other, "lime.kdl"), 'theme { frame_focus "%s" }\n' % GREEN)
    cfg = write(
        os.path.join(d, "config.kdl"),
        'theme_dir "%s"\ntheme_name "lime"\n' % other,
    )
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        check("the named directory is where it looked", frame(s) == GREEN, frame(s))


def test_a_relative_theme_dir_is_relative_to_the_file():
    """Like an include's path: a config is a thing on disk that refers to its
    neighbours, not to wherever you were standing when you started."""
    d = home()
    os.makedirs(os.path.join(d, "palettes"))
    write(
        os.path.join(d, "palettes", "lime.kdl"), 'theme { frame_focus "%s" }\n' % GREEN
    )
    cfg = write(
        os.path.join(d, "config.kdl"), 'theme_dir "palettes"\ntheme_name "lime"\n'
    )
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        check("resolved against the config, not the cwd", frame(s) == GREEN, frame(s))


def test_a_missing_theme_is_a_line_and_no_more():
    """D9: losing your keybindings over a mistyped theme name would be a worse
    answer than a session that says so."""
    d = home()
    cfg = write(os.path.join(d, "config.kdl"), 'gap 3\ntheme_name "nope"\n')
    r = subprocess.run([BIN, "--check", cfg], capture_output=True, text=True)
    check("--check says so", "no theme named nope" in r.stderr, r.stderr.strip())
    check("...and names where it looked", "themes" in r.stderr, r.stderr.strip())
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        check("the session still runs", s.alive(), "")
        check(
            "and the rest of the config applied",
            s.pane()["x"] >= 3,
            str(s.pane()["x"]),
        )


def test_a_theme_name_is_a_name_not_a_path():
    d = home()
    cfg = write(os.path.join(d, "config.kdl"), 'theme_name "../../etc/passwd"\n')
    r = subprocess.run([BIN, "--check", cfg], capture_output=True, text=True)
    check(
        "a path is refused rather than half-honoured",
        "not a path" in r.stderr,
        r.stderr.strip(),
    )
    check(
        "...and it points at include instead", "include" in r.stderr, r.stderr.strip()
    )


def test_the_control_socket_lists_and_switches():
    d = home(
        lime='theme { frame_focus "%s" }\n' % GREEN,
        rose='theme { frame_focus "%s" }\n' % PINK,
    )
    cfg = write(os.path.join(d, "config.kdl"), 'theme_name "lime"\n')
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        r = s.api("theme")
        check("it lists what is installed", r["themes"] == ["lime", "rose"], str(r))
        check("...and says which is worn", r["theme"] == "lime", str(r))
        check(
            "...and where it looked, in order",
            r["dirs"] and r["dirs"][0] == os.path.join(d, "themes"),
            str(r.get("dirs")),
        )

        s.api("theme", name="rose")
        s.settle(30)
        check("switching repaints in the new colours", frame(s) == PINK, frame(s))

        bad = s.api("theme", name="nope")
        check("an unknown name is refused", not bad["ok"], str(bad))
        s.settle(30)
        check(
            "...and costs nothing: the session keeps what it wore",
            frame(s) == PINK,
            frame(s),
        )


def test_a_switch_survives_the_next_reload():
    """The session holds the name, not the file -- otherwise saving your config
    for an unrelated reason would silently put the old colours back."""
    d = home(
        lime='theme { frame_focus "%s" }\n' % GREEN,
        rose='theme { frame_focus "%s" }\n' % PINK,
    )
    cfg = write(os.path.join(d, "config.kdl"), 'theme_name "lime"\n')
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        s.api("theme", name="rose")
        write(cfg, 'theme_name "lime"\ngap 2\n')  # an edit about something else
        s.api("reload")
        s.settle(30)
        check("the switch is still in force", frame(s) == PINK, frame(s))

        s.api("theme", name="")  # ...until it is given back
        s.settle(30)
        check("cleared, the file's own theme returns", frame(s) == GREEN, frame(s))


def test_saving_writes_one_line_and_leaves_the_rest_alone():
    d = home(
        lime='theme { frame_focus "%s" }\n' % GREEN,
        rose='theme { frame_focus "%s" }\n' % PINK,
    )
    cfg = write(
        os.path.join(d, "config.kdl"),
        '// my config\ngap 2\ntheme_name "lime"\nsplash_ms 10\n',
    )
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        r = s.api("theme", name="rose", save=True)
        check("it says it saved", r.get("saved") is True, str(r))
        text = open(cfg).read()
        check("the line is replaced", 'theme_name "rose"' in text, text)
        check("...exactly once", text.count("theme_name") == 1, text)
        check(
            "...and every other line is untouched",
            "// my config" in text and "gap 2" in text and "splash_ms 10" in text,
            text,
        )


def test_saving_into_a_config_that_never_said_anything():
    d = home(rose='theme { frame_focus "%s" }\n' % PINK)
    cfg = write(os.path.join(d, "config.kdl"), "gap 1\n")
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        s.api("theme", name="rose", save=True)
        text = open(cfg).read()
        check("the line is added", 'theme_name "rose"' in text, text)
        check("...and the file it was added to is still there", "gap 1" in text, text)


def test_a_theme_may_be_shadowed_by_one_of_yours():
    """Both directories are searched, yours first, which is how a shipped theme
    is edited without touching a file root owns."""
    d = home(default='theme { frame_focus "%s" }\n' % BLUE)
    cfg = write(os.path.join(d, "config.kdl"), 'theme_name "default"\n')
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        check("the local file is the one read", frame(s) == BLUE, frame(s))


def test_the_seed_config_offers_a_name_rather_than_a_palette():
    """`--dump-config` writes the file to begin from, and the file to begin
    from must not open with sixty colours: the whole point of a named theme is
    that the colours are somewhere a name can follow when they change."""
    d = home(lime='theme { frame_focus "%s" title "#010203" }\n' % GREEN)
    cfg = write(
        os.path.join(d, "config.kdl"),
        'theme_name "lime"\ntheme { frame_idle "%s" }\n' % PINK,
    )
    env = dict(os.environ, SLOSH_CONFIG=cfg)
    r = subprocess.run([BIN, "--check", cfg], capture_output=True, text=True, env=env)
    check("the config is clean", r.returncode == 0, r.stderr.strip())

    dump = subprocess.run(
        [BIN, "--dump-config"], capture_output=True, text=True, env=env
    ).stdout
    check(
        "it names no theme of its own",
        not any(l.startswith("theme_name") for l in dump.splitlines()),
        dump[:200],
    )
    check(
        "...but says the setting exists",
        "// theme_name" in dump and "// theme_dir" in dump,
        dump[:400],
    )
    check(
        "...and pins no palette: one example line, not a block",
        "\ntheme {" not in dump,
        dump[:400],
    )
    seeded = write(os.path.join(d, "seeded.kdl"), dump)
    r2 = subprocess.run([BIN, "--check", seeded], capture_output=True, text=True)
    check("the file it writes loads clean", r2.returncode == 0, r2.stderr.strip())

    theme = subprocess.run(
        [BIN, "--dump-theme"], capture_output=True, text=True, env=env
    ).stdout
    check("--dump-theme resolves the palette", GREEN in theme, theme[:200])
    check("...including what the config overrode", PINK in theme, theme[:200])
    check("...and says what it started from", "lime" in theme, theme[:300])


def test_a_dumped_theme_is_a_theme():
    """--dump-theme has to produce a file that can be named and worn, which is
    the only reason to write sixty colours out anywhere."""
    d = home(lime='theme { frame_focus "%s" }\n' % GREEN)
    cfg = write(os.path.join(d, "config.kdl"), 'theme_name "lime"\n')
    env = dict(os.environ, SLOSH_CONFIG=cfg)
    theme = subprocess.run(
        [BIN, "--dump-theme"], capture_output=True, text=True, env=env
    ).stdout
    write(os.path.join(d, "themes", "mine.kdl"), theme)
    write(os.path.join(d, "config2.kdl"), 'theme_name "mine"\n')
    r = subprocess.run(
        [BIN, "--check", os.path.join(d, "config2.kdl")],
        capture_output=True,
        text=True,
    )
    check("it lints as a theme", r.returncode == 0, r.stderr.strip())
    with Session(SH, cols=50, rows=10, config=os.path.join(d, "config2.kdl")) as s:
        s.settle(30)
        check("and wearing it paints the same", frame(s) == GREEN, frame(s))


def test_editing_the_theme_repaints_on_reload():
    """A theme read by the loader is a file the session was built from, so it
    is watched like every other one -- reload here, since the scripted driver
    has no watcher of its own."""
    d = home(lime='theme { frame_focus "%s" }\n' % GREEN)
    cfg = write(os.path.join(d, "config.kdl"), 'theme_name "lime"\n')
    with Session(SH, cols=50, rows=10, config=cfg) as s:
        s.settle(30)
        write(
            os.path.join(d, "themes", "lime.kdl"), 'theme { frame_focus "%s" }\n' % PINK
        )
        s.api("reload")
        s.settle(30)
        check("the edited theme is what is drawn", frame(s) == PINK, frame(s))


# ---- the picker ------------------------------------------------------------
#
# `C-a t`. The list is names, and a name says nothing about a colour scheme, so
# moving the selection wears the theme: the session behind the box is the only
# honest preview of a palette meant to dress a whole session.


def picker(s):
    """The picker's rows, as text, without the frame either side."""
    return [l.strip() for l in s.snapshot().screen().splitlines()]


def test_the_picker_lists_what_can_be_named():
    d = home(
        lime='theme { frame_focus "%s" }\n' % GREEN,
        rose='theme { frame_focus "%s" }\n' % PINK,
    )
    cfg = write(os.path.join(d, "config.kdl"), 'theme_name "lime"\n')
    with Session(SH, cols=64, rows=16, config=cfg) as s:
        s.settle(30)
        s.key("t")
        s.settle(30)
        screen = s.snapshot().screen()
        check("it is a picker called themes", "themes" in screen, screen)
        for name in ("lime", "rose"):
            check(f"{name} is listed", name in screen, screen)
        check(
            "the config's own theme says so on its row",
            "in your config" in screen,
            screen,
        )
        check(
            "and the footer says what the keys do",
            "C-s" in screen and "esc" in screen,
            screen,
        )


def test_moving_the_selection_wears_the_theme():
    d = home(
        lime='theme { frame_focus "%s" }\n' % GREEN,
        rose='theme { frame_focus "%s" }\n' % PINK,
    )
    cfg = write(os.path.join(d, "config.kdl"), 'theme_name "lime"\n')
    with Session(SH, cols=64, rows=16, config=cfg) as s:
        s.settle(30)
        before = frame(s)
        s.key("t")
        s.settle(30)
        check(
            "opening it changes nothing by itself",
            s.api("theme")["theme"] == "lime",
            str(s.api("theme")),
        )
        s.send(r"\e[B")  # the selection starts on the one being worn
        s.settle(30)
        check(
            "moving to the next one puts it on",
            s.api("theme")["theme"] == "rose",
            str(s.api("theme")),
        )
        s.send(r"\x1b")  # escape
        s.settle(30)
        check(
            "escaping puts back what was worn",
            s.api("theme")["theme"] == "lime",
            str(s.api("theme")),
        )
        check("...to the cell, not just in the reply", frame(s) == before, frame(s))


def test_enter_keeps_it_for_the_session_only():
    d = home(
        lime='theme { frame_focus "%s" }\n' % GREEN,
        rose='theme { frame_focus "%s" }\n' % PINK,
    )
    cfg = write(os.path.join(d, "config.kdl"), 'theme_name "lime"\n')
    with Session(SH, cols=64, rows=16, config=cfg) as s:
        s.settle(30)
        s.key("t")
        s.settle(30)
        s.send(r"\e[B")
        s.settle(30)
        s.send(r"\r")
        s.settle(30)
        screen = s.snapshot().screen()
        check("the picker is gone", "1 of 2" not in screen, screen)
        check("...saying it was not written down", "session only" in screen, screen)
        check("the theme stayed", frame(s) == PINK, frame(s))
        check(
            "and your config was not edited by looking at a list",
            'theme_name "lime"' in open(cfg).read(),
            open(cfg).read(),
        )


def test_ctrl_s_writes_it_down():
    d = home(
        lime='theme { frame_focus "%s" }\n' % GREEN,
        rose='theme { frame_focus "%s" }\n' % PINK,
    )
    cfg = write(os.path.join(d, "config.kdl"), '// mine\ntheme_name "lime"\ngap 1\n')
    with Session(SH, cols=64, rows=16, config=cfg) as s:
        s.settle(30)
        s.key("t")
        s.settle(30)
        s.send(r"\e[B")
        s.settle(30)
        s.send(r"\x13")  # C-s
        # A reply to wait on: `settle` is fire-and-forget, so reading the file
        # straight after it races the session still writing it.
        s.until_text("written to your config")
        text = open(cfg).read()
        check("the name is in the config now", 'theme_name "rose"' in text, text)
        check("...and the rest of the file survived", "// mine" in text, text)
        check(
            "it says so out loud",
            "written to your config" in s.snapshot().screen(),
            s.snapshot().screen(),
        )


def test_with_no_themes_it_says_where_it_looked():
    d = home()
    cfg = write(os.path.join(d, "config.kdl"), "gap 1\n")
    with Session(SH, cols=64, rows=16, config=cfg) as s:
        s.settle(30)
        s.key("t")
        s.settle(30)
        screen = s.snapshot().screen()
        check("no empty box", "themes" not in screen.split("\n")[4], screen)
        check("a toast naming the directory instead", "no themes in" in screen, screen)


if __name__ == "__main__":
    test_a_named_theme_is_read_from_the_theme_dir()
    test_the_config_beats_the_theme_it_named()
    test_a_theme_dir_of_your_own()
    test_a_relative_theme_dir_is_relative_to_the_file()
    test_a_missing_theme_is_a_line_and_no_more()
    test_a_theme_name_is_a_name_not_a_path()
    test_the_control_socket_lists_and_switches()
    test_a_switch_survives_the_next_reload()
    test_saving_writes_one_line_and_leaves_the_rest_alone()
    test_saving_into_a_config_that_never_said_anything()
    test_a_theme_may_be_shadowed_by_one_of_yours()
    test_the_seed_config_offers_a_name_rather_than_a_palette()
    test_a_dumped_theme_is_a_theme()
    test_editing_the_theme_repaints_on_reload()
    test_the_picker_lists_what_can_be_named()
    test_moving_the_selection_wears_the_theme()
    test_enter_keeps_it_for_the_session_only()
    test_ctrl_s_writes_it_down()
    test_with_no_themes_it_says_where_it_looked()
    sys.exit(report())
