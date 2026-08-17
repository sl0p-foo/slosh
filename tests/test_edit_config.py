#!/usr/bin/env python3
"""C-a e opens the config in $EDITOR, in a pane of its own.

The point is the loop: edit, save, watch the session change under you, edit
again. Anything that makes you leave the session to do it — another terminal,
or remembering where the file lives — is friction in the middle of the one
workflow the config watcher exists for.

The pane is *ephemeral*: quit the editor and it is gone. You asked for an
editor, not for a record of having had one.
"""

import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; read x']

# An "editor" that shows the file and stays up, and one that quits when told.
SHOWS = "tail -f"
QUITS = "sh -c 'read x; exit' --"


def conf(text='shell "x"\n'):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def test_it_opens_the_config_in_a_pane():
    c = conf('shell "a-marker-only-this-test-writes"\n')
    with Session(SH, cols=70, rows=24, config=c, env={"EDITOR": SHOWS}) as s:
        s.settle()
        check("one pane to start", len(s.panes()) == 1)
        s.key("e")
        snap = s.until_text("a-marker-only-this-test-writes")
        check("a second pane opened", len(s.panes()) == 2, str(s.panes()))
        check(
            "with the config file in it",
            "a-marker-only-this-test-writes" in snap.screen(),
            snap.screen(),
        )
    os.unlink(c)


def test_the_editor_pane_goes_when_the_editor_does():
    c = conf()
    with Session(SH, cols=70, rows=24, config=c, env={"EDITOR": QUITS}) as s:
        s.settle()
        s.key("e")
        s.settle(60)
        check("the editor is up", len(s.panes()) == 2, str(s.panes()))
        s.raw("q\\n")
        s.until(lambda _: len(s.panes()) == 1)
        check("quitting it closes the pane", len(s.panes()) == 1, str(s.panes()))
        check(
            "and leaves no corpse to dismiss",
            "[process exited" not in s.snapshot().screen(),
            s.snapshot().screen(),
        )
    os.unlink(c)


def test_the_config_can_name_the_editor():
    """`editor` beats $EDITOR, for the person whose $EDITOR is something they
    do not want a terminal pane to run."""
    c = conf('editor "tail -f"\nshell "picked-by-the-config"\n')
    with Session(
        SH, cols=70, rows=24, config=c, env={"EDITOR": "definitely-not-an-editor"}
    ) as s:
        s.settle()
        s.key("e")
        snap = s.until_text("picked-by-the-config")
        check(
            "the config's editor is used",
            "picked-by-the-config" in snap.screen(),
            snap.screen(),
        )
    os.unlink(c)


def test_it_is_not_written_into_a_dumped_layout():
    """A dump is the session's shape. An editor someone had open while it was
    taken is not part of that shape — restoring it would reopen their editor
    on a file they had finished with."""
    c = conf()
    with Session(SH, cols=70, rows=24, config=c, env={"EDITOR": SHOWS}) as s:
        s.settle()
        s.key("e")
        s.settle(60)
        check("the editor is up", len(s.panes()) == 2, str(s.panes()))
        dumped = s.api("dump-layout")["kdl"]
        check(
            "the dump does not carry the editor command",
            "tail -f" not in dumped,
            dumped,
        )
    os.unlink(c)


def test_no_room_says_so_rather_than_splitting_anyway():
    c = conf()
    with Session(SH, cols=70, rows=10, config=c, env={"EDITOR": SHOWS}) as s:
        s.settle()
        s.key("e")
        s.settle(40)
        check("still one pane", len(s.panes()) == 1, str(s.panes()))
        check(
            "and it said why", "no room" in s.snapshot().screen(), s.snapshot().screen()
        )
    os.unlink(c)


def test_the_cheatsheet_lists_it():
    c = conf()
    with Session(SH, cols=92, rows=40, config=c) as s:
        s.settle()
        s.send(r"\x01?")
        s.settle()
        screen = s.snapshot().screen()
        check("it is on the sheet", "edit the config" in screen, screen)
        check("under a key", "e " in screen, screen)
    os.unlink(c)


def test_the_control_api_can_do_it_too():
    c = conf('shell "from-the-api"\n')
    with Session(SH, cols=70, rows=24, config=c, env={"EDITOR": SHOWS}) as s:
        s.settle()
        r = s.api("edit-config")
        check("the api reports ok", r.get("ok") is True, str(r))
        snap = s.until_text("from-the-api")
        check("and opened it", "from-the-api" in snap.screen(), snap.screen())
    os.unlink(c)


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
