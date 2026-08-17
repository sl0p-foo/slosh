#!/usr/bin/env python3
"""`sl0ppty --check`: the config linter.

The loader is the only checker that cannot drift from the loader, so this is a
mode rather than a script. The difference between it and a session is what they
do with more than one problem: a session shows the first, because it has one
status line, and this shows all of them, because a linter that stops at the
first mistake makes you run it once per mistake.

Exit status is the contract: 0 clean, 1 with anything to say. That is what lets
it drop into an editor's compile step or a hook without glue.
"""

import os
import subprocess
import sys
import tempfile

from harness import BIN, check, report


def conf(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def run(*args, env=None):
    return subprocess.run(
        [BIN, "--check"] + list(args), capture_output=True, text=True, env=env
    )


def test_a_good_config_is_quiet_and_says_what_it_read():
    r = run(conf('gap 2\nkeys { prefix "ctrl+b" }\n'))
    check("exit 0", r.returncode == 0, r.stderr)
    check("nothing on stderr", not r.stderr.strip(), r.stderr)
    check("it says ok", ": ok" in r.stdout, r.stdout)
    check("...and which file it read", ".kdl" in r.stdout, r.stdout)
    check("...and the prefix it ended up with", "C-b" in r.stdout, r.stdout)
    check("...and how many bindings", "bindings" in r.stdout, r.stdout)


def test_it_reports_every_problem_not_just_the_first():
    """The whole reason for it. A session shows one; this shows the file."""
    r = run(
        conf(
            "keys {\n"
            '    prefix "ctrl+nosuchkey"\n'
            '    bind "nope" "zoom"\n'
            '    bind "z" "fly"\n'
            "}\n"
            "shaders {\n"
            "    bloom amount=200\n"
            "}\n"
            'theme { frame_focus "not a colour" }\n'
        )
    )
    check("exit 1", r.returncode == 1, r.stdout)
    for want in (
        "bad prefix: ctrl+nosuchkey",
        "bad key: nope",
        "unknown action: fly",
        "unknown shader: bloom",
        "bad colour for frame_focus",
    ):
        check(f"it reports {want!r}", want in r.stderr, r.stderr)
    check("and counts them", "5 problems" in r.stderr, r.stderr)


def test_every_problem_carries_a_file_and_a_line():
    r = run(conf('gap 1\n\nkeys {\n    bind "nope" "zoom"\n}\n'))
    check("the line number is the binding's", ":4: bad key: nope" in r.stderr, r.stderr)
    check("and the file is named", ".kdl:4:" in r.stderr, r.stderr)


def test_a_problem_in_an_included_file_names_that_file():
    """Otherwise a line number is a hunt through however many files."""
    d = tempfile.mkdtemp()
    with open(os.path.join(d, "theme.kdl"), "w") as f:
        f.write('theme { frame_focus "wrong" }\n')
    main = os.path.join(d, "config.kdl")
    with open(main, "w") as f:
        f.write('include "theme.kdl"\n')
    r = run(main)
    check("exit 1", r.returncode == 1, r.stdout)
    check("the included file is the one named", "theme.kdl:1:" in r.stderr, r.stderr)


def test_a_file_that_will_not_parse_says_nothing_applied():
    r = run(conf('keys {\n    bind "z" "zoom"\n'))  # never closed
    check("exit 1", r.returncode == 1, r.stdout)
    check("it says the file did not load", "not loaded" in r.stderr, r.stderr)


def test_a_missing_file_is_a_problem_not_a_crash():
    r = run("/nonexistent/sl0ppty.kdl")
    check("exit 1", r.returncode == 1, r.stdout)
    check("and it says so", "not loaded" in r.stderr or "cannot" in r.stderr, r.stderr)


def test_without_a_path_it_checks_the_config_a_session_would_read():
    good = conf("gap 3\n")
    env = dict(os.environ, SL0PPTY_CONFIG=good)
    r = subprocess.run([BIN, "--check"], capture_output=True, text=True, env=env)
    check("it read $SL0PPTY_CONFIG", good in r.stdout, r.stdout + r.stderr)
    check("and was happy with it", r.returncode == 0, r.stderr)


def test_the_dump_is_a_config_it_accepts():
    """`--dump-config` is documented as a file you could have written, so the
    file it writes has to be one the loader accepts. It was not: the dump wrote
    chords in the cheatsheet's notation (`C-a`, `S-←`) and the parser only spoke
    the config's."""
    dump = subprocess.run([BIN, "--dump-config"], capture_output=True, text=True).stdout
    path = conf(dump)
    r = run(path)
    check("a dumped config lints clean", r.returncode == 0, r.stderr or r.stdout)


if __name__ == "__main__":
    test_a_good_config_is_quiet_and_says_what_it_read()
    test_it_reports_every_problem_not_just_the_first()
    test_every_problem_carries_a_file_and_a_line()
    test_a_problem_in_an_included_file_names_that_file()
    test_a_file_that_will_not_parse_says_nothing_applied()
    test_a_missing_file_is_a_problem_not_a_crash()
    test_without_a_path_it_checks_the_config_a_session_would_read()
    test_the_dump_is_a_config_it_accepts()
    sys.exit(report())
