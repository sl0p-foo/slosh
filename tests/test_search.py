#!/usr/bin/env python3
"""Scrollback search: the bar, the matches, and the two ways out.

The machinery is lib-vt's (a GhosttySearch: the scrollback walk, reflow
survival, match tracking); what these tests pin down is ours -- the inline bar
on the pane's status row, the keyboard while it is open, the current match
shown as the selection with the viewport following it, and Enter/Escape
meaning keep-my-place / put-me-back.
"""

import sys

from harness import Session, check, report

# a shell whose output we can generate on demand
SH = ["/bin/sh", "-c", "stty raw -echo; sh"]

# the default theme's match colours (config.c): every match, and the current one
MATCH_BG = "#4d4124"
CUR_BG = "#f2c97a"


def match_cells(snap, bg):
    """Total columns tinted with a given match background."""
    return sum(st["w"] for st in snap.styles if st["bg"] == bg)


def fill(s, n=100):
    s.raw(f"seq 1 {n}\\n")
    s.until_text(str(n))


def bar(s):
    """The search bar's row, or None."""
    for row in s.snapshot().text:
        if "/ " in row and "\u2588" in row:
            return row
    return None


def test_bar_opens_and_finds():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        fill(s)
        p = s.pane()
        check("the viewport is at the bottom", "100" in s.snapshot().pane_text(p))

        s.send(r"\x01/")
        s.settle(50)
        row = bar(s)
        check(
            "the bar opens on the status row",
            row is not None,
            repr(s.snapshot().screen()[:300]),
        )

        s.send("42")
        s.settle(80)
        row = bar(s)
        check(
            "the query is echoed in the field", row and "/ 42\u2588" in row, repr(row)
        )
        check("the counter answers k of n", row and "1 of 1" in row, repr(row))
        text = s.snapshot().pane_text(p)
        check(
            "the viewport jumped to the match",
            "42" in text and "100" not in text,
            repr(text),
        )
        check(
            "and says it is in scrollback",
            "scrolled" in s.snapshot().screen(),
            repr(s.snapshot().screen()[:300]),
        )

        # The current match is tinted in the current-match colour, bold.
        snap = s.snapshot()
        cur = [st for st in snap.styles if st["bg"] == CUR_BG]
        check(
            "the current match is highlighted apart",
            len(cur) == 1 and "bold" in cur[0]["attrs"],
            str(cur),
        )
        pos = snap.find("42")
        check(
            "and it is on the matched row",
            bool(cur) and cur[0]["y"] == pos[1],
            f"{cur} vs row {pos[1] if pos else None}",
        )


def test_all_matches_are_highlighted():
    """Every occurrence on screen is tinted, not only the one you are on -- and
    the current one is a different colour, so 'this one' and 'all of them' read
    as two things."""
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        s.raw("seq 30 39\\n")  # a screen full of 3s: many visible matches
        s.until_text("39")
        s.send(r"\x01/")
        s.send("3")
        s.settle(80)
        snap = s.snapshot()
        others = match_cells(snap, MATCH_BG)
        cur = match_cells(snap, CUR_BG)
        check("more than one match is tinted", others >= 2, f"{others} in {MATCH_BG}")
        check("exactly one is the current match", cur == 1, f"{cur} in {CUR_BG}")

        # Stepping moves which one is current, not how many are lit.
        s.send(r"\e[A")
        s.settle(50)
        snap = s.snapshot()
        check(
            "stepping keeps exactly one current",
            match_cells(snap, CUR_BG) == 1,
            str(match_cells(snap, CUR_BG)),
        )


def test_stepping_and_wrap():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        fill(s)
        s.send(r"\x01/")
        s.send("9")
        s.settle(80)
        row = bar(s)
        # 9, 19..89, 90..98 and two in 99: byte matches, newest first.
        check("all matches are counted", row and "1 of 20" in row, repr(row))

        s.send(r"\e[A")  # up: older
        s.send(r"\e[A")
        s.settle(50)
        check("up steps into history", "3 of 20" in bar(s), repr(bar(s)))

        s.send(r"\e[B")  # down: newer
        s.settle(50)
        check("down steps back", "2 of 20" in bar(s), repr(bar(s)))

        s.send(r"\e[B")
        s.send(r"\e[B")  # past the newest: wraps to the oldest
        s.settle(50)
        check("stepping wraps", "20 of 20" in bar(s), repr(bar(s)))


def test_enter_keeps_the_place():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        fill(s, 200)
        p = s.pane()
        s.send(r"\x01/")
        s.send("42")
        s.settle(80)
        check("found it deep in history", "42" in s.snapshot().pane_text(p))

        s.send(r"\r")
        s.settle(50)
        check("enter closes the bar", bar(s) is None, repr(s.snapshot().screen()[:300]))
        text = s.snapshot().pane_text(p)
        check(
            "and the viewport stays on the match",
            "42" in text and "200" not in text,
            repr(text),
        )
        check("still marked as scrolled", "scrolled" in s.snapshot().screen())

        # Typing snaps back to the live view, like any typing while scrolled.
        s.send("x")
        s.settle(80)
        check(
            "typing returns to the present",
            "200" in s.snapshot().pane_text(p),
            repr(s.snapshot().pane_text(p)),
        )


def test_escape_snaps_back():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        fill(s, 200)
        p = s.pane()
        s.send(r"\x01/")
        s.send("42")
        s.settle(80)
        check("found it", "42" in s.snapshot().pane_text(p))

        s.send(r"\e")
        s.settle(120)
        check("escape closes the bar", bar(s) is None)
        text = s.snapshot().pane_text(p)
        check("and snaps back to the present", "200" in text, repr(text))
        check(
            "the highlights went with it",
            match_cells(s.snapshot(), MATCH_BG) == 0
            and match_cells(s.snapshot(), CUR_BG) == 0,
            str([st for st in s.snapshot().styles if st["bg"] in (MATCH_BG, CUR_BG)]),
        )
        check("the session is unharmed", s.api("alive")["alive"])


def test_no_matches_says_so():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        fill(s)
        s.send(r"\x01/")
        s.send("zzz")
        s.settle(80)
        check("no matches is said in words", "no matches" in bar(s), repr(bar(s)))

        # Backspacing to a needle that exists recovers.
        s.send(r"\x7f\x7f\x7f")
        s.send("42")
        s.settle(80)
        check("backspace re-searches", "1 of 1" in bar(s), repr(bar(s)))


def test_matching_is_case_insensitive():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        s.raw("printf 'Hello World\\n'\\n")
        s.until_text("Hello World")
        s.send(r"\x01/")
        s.send("hello")
        s.settle(80)
        check("ascii case does not matter", "1 of 1" in bar(s), repr(bar(s)))


def test_new_output_updates_the_count():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        fill(s)
        s.send(r"\x01/")
        s.send("77")
        s.settle(80)
        check("one match to start", "1 of 1" in bar(s), repr(bar(s)))

        # The program keeps printing while the bar is open: the search is fed
        # per composed frame, so the count follows without being asked.
        s.raw("echo 77\\n")
        s.until(lambda snap: "of 2" in (bar_row(snap) or ""))
        # The selected match is the one we were on -- now the *older* of the
        # two, since matches count from the newest.
        check("the count follows new output", "2 of 2" in bar(s), repr(bar(s)))


def bar_row(snap):
    for row in snap.text:
        if "/ " in row and "\u2588" in row:
            return row
    return None


def test_the_keyboard_belongs_to_the_bar():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        fill(s)
        s.send(r"\x01/")
        s.send("q")  # would quit the session if it fell through as C-a q's kin
        s.send("x")  # would close the pane
        s.settle(50)
        check(
            "letters land in the query, not the session",
            "/ qx\u2588" in bar(s),
            repr(bar(s)),
        )
        check(
            "the pane did not see them either",
            "qx" not in s.snapshot().pane_text(s.pane()),
        )
        s.send(r"\e")
        s.settle(120)
        check("the session is unharmed", s.api("alive")["alive"])


def test_click_away_commits():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        fill(s, 200)
        p = s.pane()
        s.send(r"\x01/")
        s.send("42")
        s.settle(80)
        check("found it", "42" in s.snapshot().pane_text(p))

        # Clicking into the pane is leaving the field: it commits (keeps the
        # viewport) rather than cancelling -- the click said done, not undo.
        s.click(p["content_x"] + 2, p["content_y"] + 2)
        s.settle(50)
        check("a click away closes the bar", bar(s) is None)
        check(
            "and keeps the viewport",
            "42" in s.snapshot().pane_text(p),
            repr(s.snapshot().pane_text(p)),
        )


def test_the_wheel_is_not_a_click():
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        fill(s)
        p = s.pane()
        s.send(r"\x01/")
        s.send("42")
        s.settle(80)
        # Scrolling the pane you are searching is part of searching it.
        s.send(rf"\e[<64;{p['content_x'] + 3};{p['content_y'] + 3}M")
        s.settle(50)
        check(
            "the wheel leaves the bar open",
            bar(s) is not None,
            repr(s.snapshot().screen()[:300]),
        )


def test_help_and_search_share_the_slash():
    """`/` is search and `?` is help -- which needs the decoder to read the
    shift out of a legacy `?` byte, since the byte carries no modifier."""
    with Session(SH, cols=70, rows=20) as s:
        s.settle()
        s.send(r"\x01?")
        s.settle(80)
        check(
            "? opens the cheatsheet",
            "then:" in s.snapshot().screen(),
            repr(s.snapshot().screen()[:200]),
        )
        s.send(r"\e")
        s.settle(120)
        s.send(r"\x01/")
        s.settle(50)
        check(
            "/ opens the search bar",
            bar(s) is not None,
            repr(s.snapshot().screen()[:300]),
        )


if __name__ == "__main__":
    test_bar_opens_and_finds()
    test_all_matches_are_highlighted()
    test_stepping_and_wrap()
    test_enter_keeps_the_place()
    test_escape_snaps_back()
    test_no_matches_says_so()
    test_matching_is_case_insensitive()
    test_new_output_updates_the_count()
    test_the_keyboard_belongs_to_the_bar()
    test_click_away_commits()
    test_the_wheel_is_not_a_click()
    test_help_and_search_share_the_slash()
    sys.exit(report())
