#!/usr/bin/env python3
"""Select-to-copy, middle-click paste, and toasts.

The clipboard lives on the *client's* machine, so a copy travels to it as
OSC 52; the session keeps its own copy as the primary selection, which is what
middle click pastes.
"""

import sys
import time
import unicodedata

from harness import Session, check, report

# Not an interactive shell: readline treats a typed ESC as a meta prefix and
# eats it, so `printf "<ESC>]52;..."` never reaches printf. A raw read loop
# passes bytes through untouched. (The same trap as the OSC 5577 tests.)
SH = ["/bin/sh", "-c", 'stty raw -echo; while IFS= read -r l; do eval "$l"; done']


def echoing(preamble):
    """A pane that prints something, then echoes whatever is sent to it."""
    return ["/bin/sh", "-c", f"{preamble}; stty raw -echo; cat"]


def emits(*seqs):
    """A pane that prints escape sequences itself, with printf doing the
    escaping — no driver quoting, no shell line editor in the way."""
    body = "".join('printf "%s";' % q.replace("%", "%%") for q in seqs)
    return ["/bin/sh", "-c", body + " sleep 5"]


def content(s, p=None):
    p = p or s.pane()
    return p["content_x"], p["content_y"]


def select(s, x0, y0, x1, y1):
    """Drag from one screen cell to another with the left button held."""
    s.send(rf"\e[<0;{x0 + 1};{y0 + 1}M")
    s.send(rf"\e[<32;{x1 + 1};{y1 + 1}M")
    s.send(rf"\e[<0;{x1 + 1};{y1 + 1}m")


def dbl_click(s, x, y, times=2):
    """`times` presses on the same cell, close enough together to be a double."""
    for _ in range(times):
        s.send(rf"\e[<0;{x + 1};{y + 1}M")
        s.send(rf"\e[<0;{x + 1};{y + 1}m")


LINE = (
    "src/app.c:1234:5: error in foo_bar(x); see https://a.b/c?d=1 "
    'or "quoted" \u65e5\u672c\u8a9e\u3067\u3059 end'
)


def column_of(line, index):
    """The screen column a string index lands on. A wide glyph takes two cells, so
    every position after one is shifted -- the arithmetic the terminal is doing, and
    the reason a test that skips it clicks somewhere else entirely."""
    return sum(
        2 if unicodedata.east_asian_width(c) in ("W", "F") else 1 for c in line[:index]
    )


def worded(s, needle, offset=0, times=2, line=LINE):
    """Double-click inside `needle` and return what ended up on the clipboard."""
    p = s.pane()
    x = p["content_x"] + column_of(line, line.index(needle) + offset)
    dbl_click(s, x, p["content_y"], times)
    s.settle(80)
    return s.api("clipboard")["text"]


def a_line(text=LINE):
    """A pane showing one line of text, from a file: a printf of this would hand
    the shell backticks and parentheses to interpret."""
    import tempfile

    f = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
    f.write(text + "\n")
    f.close()
    return echoing("cat " + f.name)


def test_select_to_copy():
    with Session(SH, cols=46, rows=10) as s:
        s.settle()
        s.raw(r'printf "hello selection world\\n"' + "\\n")
        s.settle(200)
        cx, cy = content(s)
        row = cy  # no prompt in this pane: our line is the first one

        select(s, cx, row, cx + 12, row)
        s.settle(80)
        clip = s.api("clipboard")["text"]
        check(
            "releasing the drag copies what was under it",
            clip == "hello selecti",
            repr(clip),
        )

        check(
            "and says so",
            "copied 13 chars" in s.snapshot().screen(),
            repr(s.snapshot().screen()[-120:]),
        )


def test_selection_is_visible():
    with Session(SH, cols=46, rows=10) as s:
        s.settle()
        s.raw(r'printf "highlight me\\n"' + "\\n")
        s.settle(200)
        cx, cy = content(s)
        row = cy

        s.send(rf"\e[<0;{cx + 1};{row + 1}M")
        s.send(rf"\e[<32;{cx + 9};{row + 1}M")
        s.settle(80)
        snap = s.snapshot()
        runs = [r for r in snap.styles if "inverse" in r["attrs"]]
        check("the selection is visible while dragging", runs != [], str(snap.styles))
        if runs:
            check(
                "and covers what was dragged over",
                runs[0]["y"] == row and runs[0]["w"] >= 8,
                str(runs[0]),
            )
        s.send(rf"\e[<0;{cx + 9};{row + 1}m")


def test_double_click_selects_a_word():
    """The gesture people already have: two clicks, one word, on the clipboard.

    The cases are the ones a terminal actually shows. `src/app.c:1234:5` is why
    `:` is not a separator -- an error location is one thing you meant to copy --
    and the CJK run is why a wide glyph's second cell continues the word rather
    than being a word of its own.

    Each case clicks *once*, because a second call here would be another pair of
    clicks: the assertion has to hold the answer, not ask for it twice."""
    with Session(a_line(), cols=110, rows=12) as s:
        s.until_text("error")
        for label, needle, offset, want in (
            ("an error location comes whole", "app.c", 2, "src/app.c:1234:5:"),
            ("an identifier stops at the bracket", "foo_bar", 3, "foo_bar"),
            ("a URL is one word", "https", 4, "https://a.b/c?d=1"),
            (
                "quotes separate, so the word inside them is the word",
                "quoted",
                2,
                "quoted",
            ),
            (
                "a run of wide glyphs is one word",
                "\u65e5\u672c\u8a9e",
                1,
                "\u65e5\u672c\u8a9e\u3067\u3059",
            ),
        ):
            got = worded(s, needle, offset)
            check(label, got == want, repr(got))


def test_the_highlight_is_the_copy():
    """Whatever is on the clipboard is what the screen says is selected -- the
    same rule the drag has, since it is the same selection."""
    with Session(a_line(), cols=110, rows=12) as s:
        s.until_text("error")
        clip = worded(s, "foo_bar", 3)
        snap = s.snapshot()
        runs = [r for r in snap.styles if "inverse" in r["attrs"]]
        check("exactly one highlighted run", len(runs) == 1, str(snap.styles))
        if runs:
            p = s.pane()
            check(
                "on the row that was clicked",
                runs[0]["y"] == p["content_y"],
                str(runs[0]),
            )
            check(
                "as wide as the word that was copied",
                runs[0]["w"] == len(clip),
                "%s vs %r" % (runs[0], clip),
            )


def test_a_click_that_finds_no_word_leaves_the_clipboard_alone():
    """A separator, a blank cell, or one click rather than two: nothing was named,
    so nothing is copied. Wiping a clipboard is not something a stray click should
    be able to do."""
    with Session(a_line(), cols=110, rows=12) as s:
        s.until_text("error")
        first = worded(s, "foo_bar", 3)
        check("something is on the clipboard to lose", first == "foo_bar", repr(first))

        on_separator = worded(s, "(x)")
        check(
            "double-clicking a separator copies nothing new",
            on_separator == first,
            repr(on_separator),
        )

        on_blank = worded(s, " end")
        check(
            "nor does double-clicking a blank cell", on_blank == first, repr(on_blank)
        )

        single = worded(s, "end", 1, times=1)
        check("nor does a single click", single == first, repr(single))

        runs = [r for r in s.snapshot().styles if "inverse" in r["attrs"]]
        check("and the pane shows no selection either", runs == [], str(runs))


def test_two_clicks_on_different_words_are_not_a_double_click():
    """Per cell, not per pane: clicking one word and then another quickly must not
    select everything between them."""
    with Session(a_line(), cols=110, rows=12) as s:
        s.until_text("error")
        p = s.pane()
        x1 = p["content_x"] + column_of(LINE, LINE.index("foo_bar") + 3)
        x2 = p["content_x"] + column_of(LINE, LINE.index("quoted") + 2)
        row = p["content_y"]
        dbl_click(s, x1, row, 1)
        dbl_click(s, x2, row, 1)
        s.settle(80)
        check(
            "neither click selected anything on its own",
            s.api("clipboard")["text"] in (None, ""),
            repr(s.api("clipboard")["text"]),
        )
        dbl_click(s, x2, row, 1)  # ...and the second cell's pair completes
        s.settle(80)
        check(
            "the pair that lands on one cell selects that word",
            s.api("clipboard")["text"] == "quoted",
            repr(s.api("clipboard")["text"]),
        )


def test_slow_clicks_are_two_clicks():
    with Session(a_line(), cols=110, rows=12) as s:
        s.until_text("error")
        p = s.pane()
        x = p["content_x"] + column_of(LINE, LINE.index("foo_bar") + 3)
        dbl_click(s, x, p["content_y"], 1)
        time.sleep(0.5)  # longer than double_click_ms (400)
        dbl_click(s, x, p["content_y"], 1)
        s.settle(80)
        check(
            "two slow clicks select no word",
            s.api("clipboard")["text"] in (None, ""),
            repr(s.api("clipboard")["text"]),
        )


def test_the_separators_are_configurable():
    """`word_separators` lists separators, so anything unlisted is part of a word:
    emptying it makes punctuation part of the word, up to the blanks that always
    separate whether they are listed or not."""
    import tempfile

    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write('word_separators ""\n')
    f.close()
    with Session(a_line(), cols=110, rows=12, config=f.name) as s:
        s.until_text("error")
        got = worded(s, "foo_bar", 3)
        check(
            "with no separators, punctuation is part of the word",
            got == "foo_bar(x);",
            repr(got),
        )
        check("but a space still ends it", " " not in got, repr(got))

    g = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    g.write('word_separators "_"\n')  # the opposite: split an identifier
    g.close()
    with Session(a_line(), cols=110, rows=12, config=g.name) as s:
        s.until_text("error")
        got = worded(s, "foo_bar", 1)
        check(
            "and a listed character splits what it is listed in",
            got == "foo",
            repr(got),
        )

    # The two characters that end or escape a KDL string are also the two a
    # double-click most wants to stop at, so the config path has to carry them:
    # `"quoted"` and `C:\\path` are the shapes this is for.
    q, bs = '"', chr(92)
    h = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    h.write("word_separators " + q + bs + q + bs + bs + q + chr(10))
    h.close()
    text = "say " + q + "hi" + q + " and " + bs + "esc" + bs + " ok"
    with Session(a_line(text), cols=80, rows=10, config=h.name) as s:
        s.until_text("say")
        got = worded(s, "hi", 0, line=text)
        check(
            "a quote can be a separator, escaped through the config",
            got == "hi",
            repr(got),
        )
        got = worded(s, "esc", 0, line=text)
        check("and so can a backslash", got == "esc", repr(got))


def test_a_word_in_scrollback_is_the_word_on_screen():
    """The word is found by walking the row the pointer is on, and while a pane is
    scrolled that row is not the row the terminal calls `y`. Same coordinates the
    drag selection uses -- viewport, not screen history -- so this is the check that
    they stay the same."""
    body = "; ".join(
        "printf 'line%02d unique_%02d\\r\\n'" % (i, i) for i in range(1, 41)
    )
    with Session(echoing(body), cols=60, rows=12) as s:
        s.until_text("line40")
        p = s.pane()
        for _ in range(6):  # wheel up into the scrollback
            s.send(rf"\e[<64;{p['content_x'] + 2};{p['content_y'] + 2}M")
        s.settle(60)

        row = s.snapshot().line(p["content_y"])
        word = row.split()[1]
        check("the top row is a scrollback row", word.startswith("unique_"), repr(row))
        dbl_click(s, row.index(word), p["content_y"])
        s.settle(80)
        got = s.api("clipboard")["text"]
        check(
            "and the word copied is the one on screen",
            got == word,
            "%r from %r" % (got, row.strip()),
        )


def test_a_mouse_program_keeps_its_double_clicks():
    """The same rule the drag has: a program that asked for the mouse gets the
    clicks, and shift is the way to select over one anyway. A word selection that
    ignored this would take double-clicks away from every program in every pane."""
    tracking = [
        "/bin/sh",
        "-c",
        "printf '\\033[?1002h'; printf 'tracked_word here\\r\\n'; stty raw -echo; cat",
    ]
    with Session(tracking, cols=60, rows=10) as s:
        s.until_text("tracked_word")
        p = s.pane()
        row = p["content_y"]
        col = s.snapshot().line(row).index("tracked_word")

        dbl_click(s, col, row)
        s.settle(80)
        check(
            "a plain double-click is the program's",
            s.api("clipboard")["text"] in (None, ""),
            repr(s.api("clipboard")["text"]),
        )

        for _ in range(2):  # the same pair with shift held
            s.send(rf"\e[<4;{col + 1};{row + 1}M")
            s.send(rf"\e[<4;{col + 1};{row + 1}m")
        s.settle(80)
        check(
            "shift takes it back and selects the word",
            s.api("clipboard")["text"] == "tracked_word",
            repr(s.api("clipboard")["text"]),
        )


# A line longer than the pane, so the terminal lays it across two rows. The
# string itself contains no newline: where it breaks is a fact about how wide the
# pane is, and nothing a paste should carry.
WRAPPED = "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEE"


def test_a_wrapped_line_is_copied_as_one_line():
    """A row boundary in the grid is not a newline in the text. Copying a wrapped
    path or URL used to paste a `\\n` into the middle of it, which is the kind of
    wrong that is invisible until it reaches a shell."""
    with Session(
        ["/bin/sh", "-c", 'printf "%%s\\n" "%s"; read x' % WRAPPED], cols=40, rows=10
    ) as s:
        s.settle(200)
        p = s.pane()
        cx, cy = content(s, p)
        check(
            "the pane really did wrap it",
            s.snapshot().pane_line(p, 1).strip().startswith("DDDD"),
            repr(s.snapshot().pane_line(p, 1)),
        )

        select(s, cx, cy, cx + 15, cy + 1)
        got = s.api("clipboard")["text"]
        check("the copy has no newline in it", "\n" not in got, repr(got))
        check("and it is the line that was printed", got == WRAPPED, repr(got))


def test_real_newlines_are_still_newlines():
    """The other half, and the one a fix for the first could easily break: rows
    that are separate *lines* keep the boundaries between them."""
    with Session(
        ["/bin/sh", "-c", 'printf "one\\ntwo\\nthree\\n"; read x'], cols=40, rows=10
    ) as s:
        s.settle(200)
        cx, cy = content(s)
        select(s, cx, cy, cx + 4, cy + 2)
        got = s.api("clipboard")["text"]
        check(
            "three lines come back as three lines", got == "one\ntwo\nthree", repr(got)
        )


def test_a_word_that_straddles_the_wrap_is_still_one_word():
    """Double-clicking the tail of a wrapped word selects the row's run, because a
    word selection is a run of cells on one row. What it must not do is paste a
    newline, which is the same bug in the other gesture."""
    with Session(
        ["/bin/sh", "-c", 'printf "%%s\\n" "%s"; read x' % WRAPPED], cols=40, rows=10
    ) as s:
        s.settle(200)
        cx, cy = content(s)
        dbl_click(s, cx + 2, cy)
        got = s.api("clipboard")["text"]
        check("no newline arrives with it", "\n" not in got, repr(got))
        check("and it copied something", got != "", repr(got))


def test_middle_click_pastes():
    # a pane that echoes, so a paste is visible; the eval-loop pane would try
    # to *run* what was pasted, which is a different (and worse) demonstration
    with Session(echoing(r'printf "paste-me-please\n"'), cols=46, rows=10) as s:
        s.settle(200)
        cx, cy = content(s)
        select(s, cx, cy, cx + 14, cy)
        s.settle(80)
        check(
            "something was copied",
            s.api("clipboard")["text"] == "paste-me-please",
            repr(s.api("clipboard")["text"]),
        )

        s.send(rf"\e[<1;{cx + 1};{cy + 3}M")  # middle button
        s.send(rf"\e[<1;{cx + 1};{cy + 3}m")
        s.settle(150)
        text = s.snapshot().pane_text(s.pane())
        check(
            "middle click pastes the selection into the pane",
            text.count("paste-me-please") >= 2,
            repr(text[:120]),
        )


def test_a_program_can_copy():
    """OSC 52 from inside a pane reaches the same clipboard."""
    with Session(SH, cols=50, rows=10) as s:
        s.settle()
        # base64 of "from-the-pane"
        s.raw(r'printf "\e]52;c;ZnJvbS10aGUtcGFuZQ==\x07"' + "\\n")
        s.settle(200)
        check(
            "a pane writing OSC 52 sets the clipboard",
            s.api("clipboard")["text"] == "from-the-pane",
            repr(s.api("clipboard")["text"]),
        )


def test_toasts():
    # A toast's lifetime is a real duration, so this really does wait for it -
    # but the duration is configurable, so it waits 150ms instead of 2500.
    import tempfile

    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write("toast_ms 150\n")
    f.close()
    with Session(SH, cols=50, rows=10, config=f.name) as s:
        s.settle()
        r = s.api("notify", text="build finished")
        check("notify is accepted", r["ok"], str(r))
        check(
            "and appears on screen",
            "build finished" in s.snapshot().screen(),
            repr(s.snapshot().screen()[-120:]),
        )
        check(
            "bottom right, out of the way",
            "build finished" in s.snapshot().text[-1],
            repr(s.snapshot().text[-1]),
        )

        s.api("notify", text="second thing")
        snap = s.snapshot()
        check(
            "several stack up",
            "build finished" in snap.screen() and "second thing" in snap.screen(),
            repr(snap.screen()[-200:]),
        )

        time.sleep(0.25)
        s.settle(20)
        check(
            "and they expire on their own",
            "build finished" not in s.snapshot().screen(),
            repr(s.snapshot().screen()[-120:]),
        )


def test_pane_notifications_become_toasts():
    with Session(SH, cols=50, rows=10) as s:
        s.settle()
        s.raw(r'printf "\e]9;agent needs you\x07"' + "\\n")
        s.settle(200)
        check(
            "OSC 9 from a pane becomes a toast",
            "agent needs you" in s.snapshot().screen(),
            repr(s.snapshot().screen()[-160:]),
        )


def test_selection_does_not_fight_a_mouse_program():
    tracker = [
        "/bin/sh",
        "-c",
        'stty raw -echo; printf "\\033[?1000h\\033[?1006h"; cat -v',
    ]
    with Session(tracker, cols=50, rows=10) as s:
        s.settle()
        cx, cy = content(s)
        select(s, cx + 1, cy + 1, cx + 6, cy + 1)
        s.settle(80)
        check(
            "a mouse-tracking program keeps its own clicks",
            s.api("clipboard")["text"] == "",
            repr(s.api("clipboard")["text"]),
        )
        check(
            "and receives them",
            "^[[<0;" in s.snapshot().pane_text(s.pane()),
            repr(s.snapshot().pane_text(s.pane())[:100]),
        )


if __name__ == "__main__":
    test_select_to_copy()
    test_selection_is_visible()
    test_a_wrapped_line_is_copied_as_one_line()
    test_real_newlines_are_still_newlines()
    test_a_word_that_straddles_the_wrap_is_still_one_word()
    test_middle_click_pastes()
    test_a_program_can_copy()
    test_toasts()
    test_pane_notifications_become_toasts()
    test_selection_does_not_fight_a_mouse_program()
    test_double_click_selects_a_word()
    test_the_highlight_is_the_copy()
    test_a_click_that_finds_no_word_leaves_the_clipboard_alone()
    test_two_clicks_on_different_words_are_not_a_double_click()
    test_slow_clicks_are_two_clicks()
    test_the_separators_are_configurable()
    test_a_word_in_scrollback_is_the_word_on_screen()
    test_a_mouse_program_keeps_its_double_clicks()
    sys.exit(report())
