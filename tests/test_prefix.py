#!/usr/bin/env python3
"""The leader key is whatever you say it is — everywhere.

It has always been configurable; what had not been was the badge in the status
bar, which was the string "C-a" whatever you had bound. A prefix you can
change and a UI that keeps naming the old one is worse than no setting: it
teaches you to distrust the thing that is telling you which mode you are in.

So this is mostly one assertion made in several places: rebind it, and the
badge, the cheatsheet, the swallowing and the pass-through all follow.
"""
import sys
import tempfile

from harness import Session, check, report

# `cat -v`, so a control byte that reaches the program is *visible* as ^A
# rather than arriving as an invisible control character and proving nothing.
SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat -v']


def cfg(prefix):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write('keys {\n    prefix "%s"\n}\n' % prefix)
    f.close()
    return f.name


def strip(s):
    return s.snapshot().line(1)


def test_the_badge_names_the_key_you_bound():
    for prefix, chord, shown in (("ctrl+a", r"\x01", "C-a"),
                                 ("ctrl+b", r"\x02", "C-b"),
                                 ("ctrl+space", r"\x00", "C-space"),
                                 ("alt+x", r"\ex", "M-x")):
        with Session(SH, cols=60, rows=8, config=cfg(prefix)) as s:
            s.settle()
            check(f"{prefix}: no badge until it is pressed",
                  shown not in strip(s), strip(s))
            s.send(chord)
            s.settle()
            check(f"{prefix}: the badge says {shown}", shown in strip(s),
                  strip(s))


def test_a_rebound_prefix_actually_works():
    with Session(SH, cols=80, rows=14, config=cfg("ctrl+b")) as s:
        s.settle()
        check("one pane to start", len(s.panes()) == 1)
        s.send(r"\x02\\")               # C-b \ : split
        s.settle()
        check("the new prefix runs commands", len(s.panes()) == 2,
              str(s.panes()))


def test_the_old_prefix_goes_to_the_program_instead():
    """The reason to move it: C-a is start-of-line to half the world."""
    with Session(SH, cols=80, rows=14, config=cfg("ctrl+b")) as s:
        s.settle()
        s.send(r"\x01")                 # C-a, no longer ours
        s.settle(40)
        check("C-a is not swallowed as a prefix", "C-a" not in strip(s),
              strip(s))
        snap = s.until(lambda sn: "^A" in sn.screen())
        check("it reached the program", "^A" in snap.screen(),
              snap.screen()[:200])


def test_pressing_the_prefix_twice_sends_it_on():
    with Session(SH, cols=80, rows=14, config=cfg("ctrl+b")) as s:
        s.settle()
        s.send(r"\x02\x02")
        s.settle(40)
        check("the badge is gone again", "C-b" not in strip(s), strip(s))
        snap = s.until(lambda sn: "^B" in sn.screen())
        check("and the pane got the keystroke", "^B" in snap.screen(),
              snap.screen()[:200])


def test_the_cheatsheet_agrees_with_the_badge():
    with Session(SH, cols=92, rows=28, config=cfg("ctrl+b")) as s:
        s.settle()
        s.send(r"\x02?")
        s.settle()
        screen = s.snapshot().screen()
        check("the cheatsheet names the same key", "C-b then:" in screen,
              screen)
        check("and not the old one", "C-a then:" not in screen, screen)


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
