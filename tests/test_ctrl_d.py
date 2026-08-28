#!/usr/bin/env python3
"""`ctrl_d_exits`: Ctrl-D at an idle prompt, where the tty cannot do it.

Windows has no VEOF, so ^D there is a key that does nothing. The knob makes
slosh send `exit` on the user's behalf -- but only where that is unambiguously
what ^D meant, because every other case costs a pane. Those refusals are the
interesting half, and most of what is below.

Off by default on POSIX, so the tests that want it say so. The guards are the
same code on every platform, which is why it is a knob and not an `#ifdef`
nobody outside Windows could ever run.

Telling trapped from forwarded needs care here. On POSIX a *forwarded* ^D also
ends an idle shell -- that is the line discipline doing its job -- so "did the
pane go" cannot tell the two apart, and neither can looking for the word
`exit`, which bash prints itself on EOF. PROBE settles it: a program that
reads one line and shows exactly what arrived. `exit` is what slosh types;
an empty line is EOF reaching the program untouched.
"""

import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh"]
# Reads one line, then says what it got. The sleep keeps the pane up long
# enough to be read: the answer is on screen, not in an exit status.
PROBE = ["/bin/sh", "-c", 'read x; echo "GOT:[$x]"; sleep 5']
# The same, from the alternate screen, switching back so the answer is visible.
PROBE_ALT = [
    "/bin/sh",
    "-c",
    'printf "\\033[?1049h"; read x; printf "\\033[?1049l"; echo "GOT:[$x]"; sleep 5',
]


def conf(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


ON = conf("ctrl_d_exits true\n")
OFF = conf("ctrl_d_exits false\n")
# keep_dead all: the corpse stays, so its exit status can be read.
CORPSE = conf("ctrl_d_exits true\nkeep_dead all\n")


def got(s):
    """What the probe program actually received."""
    snap = s.until_text("GOT:")
    for line in snap.screen().splitlines():
        if "GOT:[" in line:
            return line.split("GOT:[", 1)[1].split("]", 1)[0]
    return None


def gone(s):
    return len(s.panes()) == 0


def test_ctrl_d_at_an_idle_prompt_ends_the_shell():
    with Session(SH, cols=60, rows=10, config=ON) as s:
        s.settle(80)
        check("the pane is alive to begin with", len(s.panes()) == 1, str(s.panes()))
        s.send("\\x04")
        snap = s.until(lambda _: gone(s))
        check("^D closed it", gone(s), snap.screen())


def test_what_arrives_is_the_word_exit():
    with Session(PROBE, cols=60, rows=10, config=ON) as s:
        s.settle(80)
        s.send("\\x04")
        check("the pane was sent `exit`", got(s) == "exit", repr(got(s)))


def test_the_shell_exits_rather_than_being_reached_past():
    """The shell ends itself: status 0, no signal.

    Closing the pane from outside would leave no status at all, and killing the
    process would leave a signal. Both are visible here, which makes this the
    test that says *how* it was done rather than only that it was."""
    with Session(SH, cols=60, rows=10, config=CORPSE) as s:
        s.settle(80)
        s.send("\\x04")
        s.until(lambda _: not s.pane(0)["alive"])
        pane = s.pane(0)
        check("the pane is dead but kept", not pane["alive"], str(pane))
        check("it exited, and was not signalled", pane["exit_signal"] == 0, str(pane))
        check("with the shell's own status", pane["exit_code"] == 0, str(pane))


def test_off_by_default():
    with Session(PROBE, cols=60, rows=10) as s:
        s.settle(80)
        s.send("\\x04")
        check("the raw ^D is forwarded, and arrives as EOF", got(s) == "", repr(got(s)))


def test_the_knob_turns_it_off():
    with Session(PROBE, cols=60, rows=10, config=OFF) as s:
        s.settle(80)
        s.send("\\x04")
        check("off means off", got(s) == "", repr(got(s)))


def test_a_half_typed_line_is_not_thrown_away():
    """POSIX sends EOF on a line with something on it and the shell ignores it.
    What it does not do is discard the line, and neither do we -- this is the
    guard that stops a mistyped ^D costing the command you were composing."""
    with Session(SH, cols=60, rows=10, config=ON) as s:
        s.settle(80)
        s.send("echo hello")
        s.settle(60)
        s.send("\\x04")
        s.settle(120)
        check("the pane survived", len(s.panes()) == 1, str(s.panes()))
        screen = s.snapshot().screen()
        check("and the line is still there", "echo hello" in screen, screen)


def test_backspacing_the_line_away_makes_it_empty_again():
    """The count comes back down, so a typo you corrected does not wedge ^D
    off for the rest of the line."""
    with Session(SH, cols=60, rows=10, config=ON) as s:
        s.settle(80)
        s.send("ab")
        s.settle(60)
        s.send("\\x7f\\x7f")  # two backspaces
        s.settle(60)
        s.send("\\x04")
        snap = s.until(lambda _: gone(s))
        check("an emptied line exits again", gone(s), snap.screen())


def test_enter_clears_the_line():
    """Having run a command must leave the next prompt exitable."""
    with Session(SH, cols=60, rows=10, config=ON) as s:
        s.settle(80)
        s.send("echo one\\r")
        s.until_text("one")
        s.send("\\x04")
        snap = s.until(lambda _: gone(s))
        check("^D works at the next prompt", gone(s), snap.screen())


def test_a_program_in_the_foreground_keeps_its_own_eof():
    """`cat` wants the EOF itself: reading stdin to the end is the whole job.
    Swallowing ^D here would break the thing this is imitating."""
    with Session(SH, cols=60, rows=10, config=ON) as s:
        s.settle(80)
        s.send("cat\\r")
        s.settle(120)
        s.send("hi\\r")
        s.until_text("hi")
        s.send("\\x04")  # ends cat, not the shell
        s.settle(150)
        check(
            "the pane is still here: ^D went to cat",
            len(s.panes()) == 1,
            str(s.panes()),
        )
        s.send("echo back\\r")
        snap = s.until_text("back")
        check(
            "and the shell has its prompt again", "back" in snap.screen(), snap.screen()
        )


def test_the_alternate_screen_keeps_ctrl_d():
    """In an editor or a pager ^D is half a page down. The alternate screen is
    the pane stating so, rather than us guessing from a process name.

    Same program and same knob as test_what_arrives_is_the_word_exit, differing
    only by the screen it reads from -- so an empty answer here is the screen
    guard and nothing else."""
    with Session(PROBE_ALT, cols=60, rows=10, config=ON) as s:
        s.settle(120)
        s.send("\\x04")
        check("^D was forwarded, not spent", got(s) == "", repr(got(s)))


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
for path in (ON, OFF, CORPSE):
    os.unlink(path)
sys.exit(report())
