#!/usr/bin/env python3
"""The keybinding cheatsheet: C-a ? brings it up, anything puts it away.

It is built from the bindings the config actually has, not from a list of what
the defaults are — a cheatsheet that can disagree with the keyboard is worse
than none, and it would disagree the first time somebody rebound a key.
"""

import re
import subprocess
import sys
import tempfile

from harness import BIN, Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def close_mark():
    """The mark the close button is drawn with, read from the binary rather than
    written down here: `--dump-config` is generated from the code, so changing
    the default cannot leave this test asserting a glyph nothing draws."""
    out = subprocess.run([BIN, "--dump-config"], capture_output=True, text=True).stdout
    m = re.search(r'close_mark "(.+)"', out)
    return m.group(1) if m else "x"


MARK = close_mark()


def open_help(s):
    s.send(r"\x01?")
    s.settle()
    return s.snapshot()


def test_it_opens_and_lists_the_bindings():
    # Tall enough for the whole sheet: this test is about what the sheet lists,
    # and truncation has its own test below. A binding added to the defaults
    # should not fail this one by pushing a heading off the bottom.
    with Session(SH, cols=92, rows=36) as s:
        s.settle()
        check(
            "nothing is up to begin with",
            "split into columns" not in s.snapshot().screen(),
        )

        snap = open_help(s)
        screen = snap.screen()
        check("the cheatsheet is up", "split into columns" in screen, screen)
        check("it says what the prefix is", "C-a then:" in screen, screen)
        # Found by shape rather than by row: where it lands is centring, and a
        # test that pins the row is testing the terminal size.
        check(
            "it is titled like a pane, in its own top border",
            any("keys" in l and "\u256d" in l for l in snap.text),
            screen,
        )
        check(
            "groups its bindings",
            all(
                g in screen
                for g in ("panes", "focus", "size", "tabs", "scroll", "session")
            ),
            screen,
        )
        check(
            "names keys the way you type them", "\\" in screen and "?" in screen, screen
        )
        check("merges the nine tab digits into one row", "1…9" in screen, screen)
        check("shows arrows as arrows", "←" in screen and "→" in screen, screen)


def style(snap, x, y):
    run = snap.style_at(x, y)
    return (run or {}).get("fg"), (run or {}).get("bg")


def modal_top(snap):
    for y, line in enumerate(snap.text):
        if "keys" in line and "\u256d" in line:
            return y, line
    return None, None


def test_sections_are_separated_by_a_blank_line():
    """Five short lists rather than one wall of rows. The line above a heading
    is blank -- except the first, which the caption already separates."""
    # Narrow, so one column and easy to read -- and tall enough for all of it,
    # because this is about the spacing between groups and not about what a short
    # terminal does with the overflow (which is checked below).
    with Session(SH, cols=56, rows=60) as s:
        s.settle()
        snap = open_help(s)
        text = [l for l in snap.text if "│" in l]

        def row_of(needle):
            for i, l in enumerate(snap.text):
                if needle in l:
                    return i
            return None

        for heading in ("focus", "size", "tabs", "scroll", "session"):
            y = row_of("  " + heading + " ")
            check(
                f"a blank line above {heading}",
                y is not None
                and heading not in snap.text[y - 1]
                and not snap.text[y - 1].strip("│╭╮╰╯─ "),
                repr(snap.text[y - 1]) if y else "not found",
            )

        first = row_of("  panes ")
        check(
            "but not above the first, which the caption already spaces",
            first is not None and "C-a then:" in snap.text[first - 2],
            repr(snap.text[first - 2]) if first else "not found",
        )


def test_the_box_is_not_taller_than_its_contents():
    """The fold falls on a heading, which strands the blank row above it at the
    bottom of the first column -- a row that draws nothing and made the box a
    line taller than it needed to be."""
    with Session(SH, cols=92, rows=38) as s:
        s.settle()
        snap = open_help(s)
        bottom = None
        for i, l in enumerate(snap.text):
            if "any key closes this" in l:
                bottom = i
        check("the footer is on the bottom border", bottom is not None, snap.screen())
        above = snap.text[bottom - 1]
        check(
            "exactly one blank line above it", not above.strip("│╭╮╰╯─ "), repr(above)
        )
        check(
            "and content on the line above that",
            snap.text[bottom - 2].strip("│╭╮╰╯─ ") != "",
            repr(snap.text[bottom - 2]),
        )


def test_the_close_button_can_be_seen_without_hovering_it():
    """It was drawn in the pane button colour on the finder background, which
    are the same value: invisible until the pointer happened to land on it."""
    with Session(SH, cols=92, rows=28) as s:
        s.settle()
        snap = open_help(s)
        y, line = modal_top(snap)
        check("the modal has a top border", y is not None, snap.screen())
        x = line.rindex(MARK)
        fg, bg = style(snap, x, y)
        check(
            "the close button is not its own background", fg != bg, f"fg {fg} bg {bg}"
        )
        check(
            "and it is registered as a target",
            snap.hit_at(x, y) == "closehelp",
            str(snap.hit_at(x, y)),
        )


def test_the_close_button_closes_it():
    with Session(SH, cols=92, rows=28) as s:
        s.settle()
        snap = open_help(s)
        y, line = modal_top(snap)
        s.click(line.rindex(MARK), y)
        s.settle()
        check(
            "clicking the button puts it away",
            "split into columns" not in s.snapshot().screen(),
        )


def test_what_is_behind_is_pushed_back():
    with Session(SH, cols=100, rows=30) as s:
        s.settle()
        before = s.snapshot()
        pos = before.find("pane 1/1")
        check("the status line is there", pos is not None, before.screen())
        x, y = pos[0] + 1, pos[1]
        lit = style(before, x, y)

        after = open_help(s)
        dimmed = style(after, x, y)
        check(
            "the screen behind the modal is dimmed", dimmed != lit, f"{lit} -> {dimmed}"
        )

        # And the modal itself is not: it is drawn after the scrim, which is
        # the whole point of the ordering. Sampled on the title rather than on
        # a corner, because the modal's top row can sit on a pane's border row
        # and then the line has two of those.
        my, line = modal_top(after)
        tx = line.index("keys")
        check(
            "the modal is not dimmed with it",
            style(after, tx, my)[0] == "#ffffff",
            str(style(after, tx, my)),
        )


def test_the_scrim_can_be_turned_off():
    conf = cfg("modal_scrim 0\n")
    with Session(SH, cols=100, rows=30, config=conf) as s:
        s.settle()
        before = s.snapshot()
        x, y = before.find("pane 1/1")[0] + 1, before.find("pane 1/1")[1]
        lit = style(before, x, y)
        after = open_help(s)
        check(
            "nothing behind it changes",
            style(after, x, y) == lit,
            f"{lit} -> {style(after, x, y)}",
        )
        check("but the modal is still up", "split into columns" in after.screen())


def test_any_key_dismisses_it_and_does_nothing_else():
    with Session(SH, cols=92, rows=28) as s:
        s.settle()
        panes = len(s.panes())
        open_help(s)

        # `\` is the split binding; while the sheet is up it must only close it.
        s.send(r"\\")
        s.settle()
        check(
            "a keystroke closes it",
            "split into columns" not in s.snapshot().screen(),
            s.snapshot().screen(),
        )
        check(
            "and is swallowed rather than run", len(s.panes()) == panes, str(s.panes())
        )


def test_a_click_dismisses_it_and_hits_nothing_underneath():
    with Session(SH, cols=92, rows=28) as s:
        s.settle()
        panes = len(s.panes())
        open_help(s)
        s.click(4, 3)  # over the pane border, under the modal
        s.settle()
        check("a click closes it", "split into columns" not in s.snapshot().screen())
        check(
            "and does not reach the border it landed on",
            len(s.panes()) == panes,
            str(s.panes()),
        )


def test_the_prefix_key_is_toggled_off_again():
    with Session(SH, cols=92, rows=28) as s:
        s.settle()
        open_help(s)
        s.send(r"\x01?")
        s.settle()
        check(
            "pressing it again puts it away",
            "split into columns" not in s.snapshot().screen(),
        )


def test_it_reports_the_bindings_the_config_has():
    """Rebound keys show up rebound, and a removed one is not advertised."""
    # A config's keys block *adds to* the defaults, so unbinding is how you
    # take something away -- and the sheet has to follow both.
    conf = cfg(
        "keys {\n"
        '    prefix "ctrl+b"\n'
        '    bind "v" "split-cols"\n'
        '    bind "-" "none"\n'
        '    bind "?" "help"\n'
        "}\n"
    )
    with Session(SH, cols=92, rows=28, config=conf) as s:
        s.settle()
        s.send(r"\x02?")  # C-b, the prefix this config asked for
        s.settle()
        screen = s.snapshot().screen()
        check(
            "the rebound key is the one shown",
            "v" in screen and "split into columns" in screen,
            screen,
        )
        check("the prefix shown is the configured one", "C-b then:" in screen, screen)
        check(
            "an unbound action is not advertised",
            "split into rows" not in screen,
            screen,
        )
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
        check(
            "the frame survives the contents",
            all(l.endswith("│") for l in body),
            str(body),
        )


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
