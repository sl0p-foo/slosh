#!/usr/bin/env python3
"""`sl0ppty --dump-config`: every setting, with its default value.

Generated from the code rather than kept as a file on disk, because a
checked-in copy of the defaults is a second source of truth and drifts — ours
had already lost four theme colours before anyone noticed.

Two properties make it worth trusting, and both are cheap to check: what it
writes must parse back to exactly what it wrote, and every key the parser
knows must appear in it. The second is the one that catches "added a knob,
forgot to render it", which is the failure this whole approach exists to
prevent.
"""

import os
import re
import subprocess
import sys
import tempfile

from harness import BIN, Session, check, report

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "config.c")


def dump(config=None):
    env = dict(os.environ)
    env["SL0PPTY_CONFIG"] = config or "/nonexistent/sl0ppty.kdl"
    return subprocess.run(
        [BIN, "--dump-config"], capture_output=True, text=True, env=env
    ).stdout


def test_it_dumps_something_that_looks_like_a_config():
    text = dump()
    check("it has content", len(text.splitlines()) > 100, str(len(text)))
    for expect in ("gap ", "theme {", "states {", "keys {", "prefix "):
        check(f"it has {expect!r}", expect in text, text[:200])


def test_it_parses_back_to_exactly_itself():
    """The round trip: dump, load that, dump again. If the two differ then one
    of them is losing something, and this is the only way to find out which
    without reading 180 lines every time."""
    one = dump()
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(one)
    f.close()
    two = dump(f.name)
    check(
        "a dump of a dump is the same dump",
        one == two,
        "\n".join(
            l
            for l in __import__("difflib").unified_diff(
                one.splitlines(), two.splitlines(), "first", "second", lineterm=""
            )
        )[:1500],
    )
    os.unlink(f.name)


def test_a_value_with_a_quote_in_it_is_written_escaped():
    """`\\` and `"` both end or escape a KDL string, so a value carrying either has
    to be written escaped or the dump stops being a file that loads. Not
    hypothetical: the default `word_separators` opens with a quote, because the
    quote is the character a double-click most wants to stop at.

    The round-trip tests above would catch a *default* that broke this. This one
    says which line, so the failure names the setting instead of the whole file."""
    dumped = dump()
    line = [l for l in dumped.splitlines() if l.startswith("word_separators ")]
    check("word_separators is rendered", len(line) == 1, str(line))
    if line:
        value = line[0].split(None, 1)[1]
        check(
            "its quote is escaped", value.startswith('"' + chr(92) + '"'), repr(value)
        )
        check(
            "and the line is one KDL string, not two",
            value.count('"') - value.count(chr(92) + '"') == 2,
            repr(value),
        )


def test_every_key_the_parser_knows_is_in_it():
    """The drift check. `kdl_child(root, "x")` is how config.c reads a
    top-level setting, so that list is the parser's own vocabulary."""
    src = open(SRC).read()
    keys = set(re.findall(r'kdl_child\(root, "([a-z_]+)"\)', src))
    # These are containers whose contents are rendered, not scalars.
    keys -= {"keys"}
    text = dump()
    missing = sorted(
        k for k in keys if not re.search(r'^(// )?%s[ {"]' % re.escape(k), text, re.M)
    )
    check(
        f"all {len(keys)} settings are rendered",
        not missing,
        "missing: " + ", ".join(missing),
    )


def test_every_theme_colour_is_in_it():
    src = open(SRC).read()
    colours = set(re.findall(r'\{"([a-z_]+)", offsetof\(config_t', src))
    text = dump()
    missing = sorted(c for c in colours if f"    {c} " not in text)
    check(
        f"all {len(colours)} colours are rendered",
        not missing,
        "missing: " + ", ".join(missing),
    )
    check("and there are plenty of them", len(colours) > 40, str(len(colours)))


def test_the_dump_is_a_working_config():
    """Not just parseable: a session started with it behaves like one started
    with nothing, because that is what "these are the defaults" means."""
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(dump())
    f.close()
    sh = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; read x']
    with Session(sh, cols=70, rows=14, config=f.name) as s:
        s.settle(20)
        with_dump = s.snapshot().screen()
    with Session(sh, cols=70, rows=14) as s:
        s.settle(20)
        with_none = s.snapshot().screen()
    check(
        "a session run from the dump looks like one run from nothing",
        with_dump == with_none,
        "\n--- from the dump\n%s\n--- from nothing\n%s" % (with_dump, with_none),
    )

    # ...and it loads without a word of complaint, which the screen alone could
    # not tell you: for a long time the dump wrote every chord in the
    # cheatsheet's notation (`prefix "C-a"`, `bind "S-←"`) and the parser only
    # spoke the config's, so eleven of its own lines came back as complaints and
    # the session ran on the defaults those lines were describing. Two sessions
    # looked identical for the worst possible reason.
    with Session(sh, cols=70, rows=14, config=f.name) as s:
        s.settle(20)
        reply = s.api("reload")
        check(
            "and it loads with nothing to complain about",
            reply.get("ok") and "warning" not in reply,
            str(reply),
        )
    os.unlink(f.name)


def test_edit_config_writes_it_when_there_is_none():
    """The reason it exists: C-a e on a fresh install opened an empty buffer,
    which tells you nothing about what you can set."""
    home = tempfile.mkdtemp(prefix="sl0ppty-home-")
    sh = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; read x']
    # No config, and no ~/.config either: a fresh container has neither.
    with Session(
        sh,
        cols=74,
        rows=24,
        env={"HOME": home, "EDITOR": "tail -f"},
        config=os.path.join(home, ".config", "sl0ppty", "config.kdl"),
    ) as s:
        s.settle()
        s.key("e")
        # `tail -f` shows the *end* of the file, so wait for something that is
        # actually on screen rather than for the first thing in the config.
        snap = s.until_text("select-tab-9")
        check(
            "the editor pane has the config in it",
            "select-tab-9" in snap.screen(),
            snap.screen()[-400:],
        )

    path = os.path.join(home, ".config", "sl0ppty", "config.kdl")
    check("and it was written, directories and all", os.path.exists(path), path)
    if os.path.exists(path):
        check(
            "with the real defaults in it",
            "theme {" in open(path).read(),
            open(path).read()[:200],
        )


def test_it_never_overwrites_a_config_you_already_have():
    home = tempfile.mkdtemp(prefix="sl0ppty-home-")
    path = os.path.join(home, "config.kdl")
    open(path, "w").write("// mine\ngap 3\n")
    sh = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; read x']
    with Session(
        sh, cols=74, rows=24, config=path, env={"HOME": home, "EDITOR": "tail -f"}
    ) as s:
        s.settle()
        s.key("e")
        s.settle(60)
        check(
            "the file is untouched",
            open(path).read() == "// mine\ngap 3\n",
            open(path).read(),
        )


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
