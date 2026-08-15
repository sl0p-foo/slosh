#!/usr/bin/env python3
"""Bindings that fire without the leader.

Every binding used to need the prefix first, which is the safe design: a
multiplexer that takes chords away from the programs inside it is a
multiplexer you fight. But it is the user's keyboard, and somebody who wants
ctrl+alt+arrow to move between panes should be allowed to have it — including
the part where that chord no longer reaches anything else.

So the deal is exactly this, and it is what these tests pin: a `direct` block
fires without the prefix, and what it fires on, nothing else sees.
"""
import sys
import tempfile

from harness import Session, check, report

# `cat -v` renders control bytes visibly, so "did the program see it" is a
# question the screen can answer.
SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat -v']

C_M_RIGHT = r"\e[1;7C"      # ctrl+alt+right
C_M_LEFT = r"\e[1;7D"       # ctrl+alt+left, deliberately left unbound


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


DIRECT = cfg('keys {\n'
             '    direct {\n'
             '        bind "ctrl+alt+right" "split-cols"\n'
             '    }\n'
             '}\n')


def test_it_fires_with_no_leader():
    with Session(SH, cols=80, rows=14, config=DIRECT) as s:
        s.settle()
        check("one pane to start", len(s.panes()) == 1)
        s.send(C_M_RIGHT)
        s.settle()
        check("the chord split the pane on its own", len(s.panes()) == 2,
              str(s.panes()))


def test_what_it_fires_on_the_program_never_sees():
    """The footgun, stated: a direct binding takes that chord away."""
    with Session(SH, cols=80, rows=14, config=DIRECT) as s:
        s.settle()
        s.send(C_M_RIGHT)
        s.settle(40)
        check("the pane was not sent the keystroke",
              "1;7C" not in s.snapshot().screen(), s.snapshot().screen()[:300])


def test_everything_else_still_reaches_the_program():
    with Session(SH, cols=80, rows=14, config=DIRECT) as s:
        s.settle()
        s.send(C_M_LEFT)
        snap = s.until(lambda sn: "1;7D" in sn.screen())
        check("an unbound chord goes straight through",
              "1;7D" in snap.screen(), snap.screen()[:300])
        check("and did nothing else", len(s.panes()) == 1, str(s.panes()))


def test_it_also_works_after_the_leader():
    """Pressing the prefix first must not make a binding stop working — that
    would be a rule nobody could keep track of."""
    with Session(SH, cols=80, rows=14, config=DIRECT) as s:
        s.settle()
        s.send(r"\x01" + C_M_RIGHT)
        s.settle()
        check("the leader then the chord does the same thing",
              len(s.panes()) == 2, str(s.panes()))


def test_prefixed_bindings_are_unaffected():
    with Session(SH, cols=80, rows=14, config=DIRECT) as s:
        s.settle()
        s.send(r"\x01\\")
        s.settle()
        check("C-a \\ still splits", len(s.panes()) == 2, str(s.panes()))


def test_the_leader_itself_cannot_be_stolen():
    """A direct binding on the prefix key would make the prefix unusable, and
    the ordering has to guarantee it cannot."""
    conf = cfg('keys {\n'
               '    prefix "ctrl+a"\n'
               '    direct { bind "ctrl+a" "close-pane" }\n'
               '}\n')
    with Session(SH, cols=80, rows=14, config=conf) as s:
        s.settle()
        s.send(r"\x01")
        s.settle()
        check("the prefix is still the prefix", "C-a" in s.snapshot().line(1),
              s.snapshot().line(1))
        check("and the pane was not closed", len(s.panes()) == 1, str(s.panes()))


def test_an_overlay_still_owns_the_keyboard():
    """While a modal is up, a direct binding must not fire underneath it."""
    with Session(SH, cols=92, rows=28, config=DIRECT) as s:
        s.settle()
        s.send(r"\x01?")                 # the cheatsheet
        s.settle()
        s.send(C_M_RIGHT)
        s.settle()
        check("the modal swallowed it", len(s.panes()) == 1, str(s.panes()))
        check("and closed, as any key does",
              "split into columns" not in s.snapshot().screen())


def test_the_cheatsheet_lists_them_apart():
    with Session(SH, cols=96, rows=30, config=DIRECT) as s:
        s.settle()
        s.send(r"\x01?")
        s.settle()
        screen = s.snapshot().screen()
        check("under a heading of their own",
              "without the leader" in screen, screen)
        check("named the way you press them", "C-M-→" in screen, screen)


def test_none_unbinds_a_direct_binding():
    conf = cfg('keys {\n'
               '    direct {\n'
               '        bind "ctrl+alt+right" "split-cols"\n'
               '        bind "ctrl+alt+right" "none"\n'
               '    }\n'
               '}\n')
    with Session(SH, cols=80, rows=14, config=conf) as s:
        s.settle()
        s.send(C_M_RIGHT)
        snap = s.until(lambda sn: "1;7C" in sn.screen())
        check("it does not fire", len(s.panes()) == 1, str(s.panes()))
        check("and the chord goes back to the program", "1;7C" in snap.screen(),
              snap.screen()[:300])


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
