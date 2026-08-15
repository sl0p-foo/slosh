#!/usr/bin/env python3
"""The keybinding cheatsheet: C-a ? brings it up, anything puts it away.

It is built from the bindings the config actually has, not from a list of what
the defaults are — a cheatsheet that can disagree with the keyboard is worse
than none, and it would disagree the first time somebody rebound a key.
"""
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def open_help(s):
    s.send(r"\x01?")
    s.settle()
    return s.snapshot()


def test_it_opens_and_lists_the_bindings():
    with Session(SH, cols=92, rows=28) as s:
        s.settle()
        check("nothing is up to begin with", "split into columns" not in
              s.snapshot().screen())

        snap = open_help(s)
        screen = snap.screen()
        check("the cheatsheet is up", "split into columns" in screen, screen)
        check("it says what the prefix is", "C-a then:" in screen, screen)
        # Found by shape rather than by row: where it lands is centring, and a
        # test that pins the row is testing the terminal size.
        check("it is titled like a pane, in its own top border",
              any("keys" in l and "\u256d" in l for l in snap.text), screen)
        check("groups its bindings", all(g in screen for g in
              ("panes", "focus", "size", "tabs", "scroll", "session")), screen)
        check("names keys the way you type them",
              "\\" in screen and "?" in screen, screen)
        check("merges the nine tab digits into one row", "1…9" in screen, screen)
        check("shows arrows as arrows", "←" in screen and "→" in screen, screen)


def test_any_key_dismisses_it_and_does_nothing_else():
    with Session(SH, cols=92, rows=28) as s:
        s.settle()
        panes = len(s.panes())
        open_help(s)

        # `\` is the split binding; while the sheet is up it must only close it.
        s.send(r"\\")
        s.settle()
        check("a keystroke closes it",
              "split into columns" not in s.snapshot().screen(),
              s.snapshot().screen())
        check("and is swallowed rather than run", len(s.panes()) == panes,
              str(s.panes()))


def test_a_click_dismisses_it_and_hits_nothing_underneath():
    with Session(SH, cols=92, rows=28) as s:
        s.settle()
        panes = len(s.panes())
        open_help(s)
        s.click(4, 3)                 # over the pane border, under the modal
        s.settle()
        check("a click closes it",
              "split into columns" not in s.snapshot().screen())
        check("and does not reach the border it landed on",
              len(s.panes()) == panes, str(s.panes()))


def test_the_prefix_key_is_toggled_off_again():
    with Session(SH, cols=92, rows=28) as s:
        s.settle()
        open_help(s)
        s.send(r"\x01?")
        s.settle()
        check("pressing it again puts it away",
              "split into columns" not in s.snapshot().screen())


def test_it_reports_the_bindings_the_config_has():
    """Rebound keys show up rebound, and a removed one is not advertised."""
    # A config's keys block *adds to* the defaults, so unbinding is how you
    # take something away -- and the sheet has to follow both.
    conf = cfg('keys {\n'
               '    prefix "ctrl+b"\n'
               '    bind "v" "split-cols"\n'
               '    bind "-" "none"\n'
               '    bind "?" "help"\n'
               '}\n')
    with Session(SH, cols=92, rows=28, config=conf) as s:
        s.settle()
        s.send(r"\x02?")            # C-b, the prefix this config asked for
        s.settle()
        screen = s.snapshot().screen()
        check("the rebound key is the one shown",
              "v" in screen and "split into columns" in screen, screen)
        check("the prefix shown is the configured one", "C-b then:" in screen,
              screen)
        check("an unbound action is not advertised",
              "split into rows" not in screen, screen)
        check("but the ones still bound are", "next tab" in screen, screen)


def test_it_fits_in_a_small_terminal_without_writing_over_its_own_frame():
    with Session(SH, cols=30, rows=10) as s:
        s.settle()
        snap = open_help(s)
        rows = [l for l in snap.text if "│" in l or "╰" in l]
        check("something is drawn", rows, snap.screen())
        # Every row of the modal ends with its own border: a label that ran on
        # would have overwritten it.
        body = [l.rstrip() for l in snap.text if "C-a then:" in l or "panes" in l]
        check("the frame survives the contents",
              all(l.endswith("│") for l in body), str(body))


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
