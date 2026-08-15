#!/usr/bin/env python3
"""M5: the status bar and the pane finder.

Tabs stop being navigation somewhere around six projects; the finder is what
replaces them at that scale.
"""
import sys

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;shell\\007"; stty raw -echo; cat']


def test_status_bar():
    with Session(SH, cols=70, rows=14) as s:
        s.settle()
        snap = s.snapshot()
        bar = snap.line(1)
        check("the bar shows a pane count", "1 pane" in bar, repr(bar))
        # Found through the hit list rather than by its label: what the
        # button says is the config's business (`newtab_mark`), and a test
        # that spells it out fails the day somebody changes their mind.
        btn = [h for h in snap.hits if h["action"] == "newtab"]
        check("the bar shows a new-tab button", len(btn) == 1, str(snap.hits))
        x = btn[0]["x"] + 1
        check("the new-tab button is clickable", snap.hit_at(x, 1) == "newtab",
              str(snap.hit_at(x, 1)))
        s.click(x, 1)
        s.settle()
        check("clicking + makes a tab", len(s.tabs()) == 2, str(s.tabs()))

        s.key("\\\\")
        s.settle()
        check("the pane count follows", "3 panes" in s.snapshot().line(1),
              repr(s.snapshot().line(1)))

    with Session(SH, cols=70, rows=14) as s:
        s.settle()
        s.send(r"\x01")  # prefix, and then nothing
        s.settle(60)
        check("a held prefix is shown as a mode", "C-a" in s.snapshot().line(1),
              repr(s.snapshot().line(1)))
        s.send("q")  # release it without quitting the session... 'q' quits
        s.settle(60)


def setup_panes(s):
    """Three panes with distinguishable purposes across two tabs."""
    s.settle()
    a = s.pane()["id"]
    s.api("set-purpose", target="pane", id=a, purpose="agent:main", declared=True)
    r = s.api("new-tab", name="build")
    b = s.panes()[-1]["id"]
    s.api("set-purpose", target="pane", id=b, purpose="service:web", declared=True)
    s.settle()
    return a, b


# The finder is a modal now, so "is it open" is asked the way it is asked of
# any modal: it owns rows in the hit list. Not by looking for a word on the
# screen, which is what the old tests did and which broke the moment the box
# got a title instead of a "find:" prompt.
def is_open(s):
    snap = s.snapshot()
    return any(h["action"].startswith("find:") or h["action"] == "closefind"
               for h in snap.hits)


def counter(s):
    """(selected, total) from the footer, or None when it is not shown."""
    import re
    for line in s.snapshot().screen().split("\n"):
        m = re.search(r"(\d+) of (\d+)", line)
        if m:
            return int(m.group(1)), int(m.group(2))
    return None


def rows(s):
    """The finder's result rows, top to bottom, as (action, y, text)."""
    snap = s.snapshot()
    out = []
    for h in snap.hits:
        if h["action"].startswith("find:"):
            out.append((h["action"], h["y"], snap.text[h["y"]]))
    return sorted(out, key=lambda r: r[1])


def test_finder_keyboard():
    with Session(SH, cols=70, rows=16) as s:
        a, b = setup_panes(s)

        s.key("f")
        s.settle(80)
        snap = s.snapshot()
        check("the finder opens", is_open(s), repr(snap.screen()[:200]))
        check("it lists panes from every tab",
              "agent:main" in snap.screen() and "service:web" in snap.screen(),
              repr(snap.screen()))
        check("and says how many it found", counter(s) == (1, 2), str(counter(s)))

        s.send("web")
        s.settle(80)
        snap = s.snapshot()
        check("typing filters the list", "service:web" in snap.screen()
              and "agent:main" not in snap.screen(), repr(snap.screen()))
        check("the query is shown as you type", "web" in snap.screen(),
              repr(snap.screen()))
        check("and the count follows it", counter(s) == (1, 1), str(counter(s)))

        s.send(r"\r")
        s.settle(80)
        check("enter focuses the match", s.focused()["id"] == b,
              str(s.focused()))
        check("and closes the finder", not is_open(s))

    with Session(SH, cols=70, rows=16) as s:
        setup_panes(s)
        s.key("f")
        s.send("zzzz")
        s.settle(80)
        check("a query matching nothing is not fatal", is_open(s))
        check("and says so rather than showing an empty box",
              "no pane matches" in s.snapshot().screen(),
              repr(s.snapshot().screen()[:400]))
        s.send(r"\x7f\x7f\x7f\x7f")  # backspace back to empty
        s.settle(80)
        check("backspace restores the list", "agent:main" in s.snapshot().screen(),
              repr(s.snapshot().screen()[:200]))
        s.send(r"\e")
        s.settle(120)
        check("escape closes it", not is_open(s))
        check("and the session is unharmed", s.api("alive")["alive"])


def test_finder_navigation():
    """Arrows, the emacs pair, tab, and the ends -- a picker in this shape is
    reached for by hands that already know one."""
    with Session(SH, cols=70, rows=16) as s:
        setup_panes(s)
        s.key("f")
        s.settle(80)
        check("it starts on the first row", counter(s) == (1, 2), str(counter(s)))

        for keys, want, what in ((r"\e[B", 2, "down"),
                                 (r"\e[A", 1, "up"),
                                 (r"\x0e", 2, "C-n"),
                                 (r"\x10", 1, "C-p"),
                                 (r"\t", 2, "tab"),
                                 (r"\e[Z", 1, "shift-tab"),
                                 (r"\e[F", 2, "end"),
                                 (r"\e[H", 1, "home")):
            s.send(keys)
            s.settle(60)
            check(f"{what} moves the selection",
                  counter(s) == (want, 2), f"{what}: {counter(s)}")

        # Wrapping: one press to reach the last row, which is most of why a
        # short list wraps at all.
        s.send(r"\e[A")
        s.settle(60)
        check("up from the first row wraps to the last",
              counter(s) == (2, 2), str(counter(s)))
        s.send(r"\e[B")
        s.settle(60)
        check("and down from the last wraps to the first",
              counter(s) == (1, 2), str(counter(s)))


def test_finder_query_editing():
    with Session(SH, cols=70, rows=16) as s:
        setup_panes(s)
        s.key("f")
        s.send("web")
        s.settle(80)
        check("the query narrows to one", counter(s) == (1, 1), str(counter(s)))
        s.send(r"\x15")  # C-u
        s.settle(80)
        check("C-u clears the query rather than the finder", is_open(s))
        check("and the whole list is back", counter(s) == (1, 2), str(counter(s)))

        # A character, not a byte. The rename editor learned this first; the
        # finder was still deleting one byte of a multi-byte character and
        # leaving the rest.
        s.send(r"\xc3\xa9")   # e-acute, two bytes of UTF-8
        s.settle(60)
        check("a multi-byte character is accepted",
              "\u00e9" in s.snapshot().screen(), repr(s.snapshot().screen()[:300]))
        s.send(r"\x7f")
        s.settle(60)
        check("and one backspace removes all of it",
              "\u00e9" not in s.snapshot().screen() and counter(s) == (1, 2),
              repr(s.snapshot().screen()[:300]))


def test_finder_mouse():
    with Session(SH, cols=70, rows=16) as s:
        a, b = setup_panes(s)
        s.api("select-tab", index=1)
        s.settle()
        s.key("f")
        s.settle(80)
        snap = s.snapshot()
        pos = snap.find("service:web")
        check("the other tab's pane is listed", pos is not None, repr(snap.screen()))
        if not pos:
            return
        x, y = pos
        check("finder rows sit on top of the hit list",
              snap.hit_at(x, y) == f"find:{b}", str(snap.hit_at(x, y)))
        s.click(x, y)
        s.settle(80)
        check("clicking a row focuses that pane", s.focused()["id"] == b,
              str(s.focused()))
        check("and switches to its tab", s.tabs()[1]["active"], str(s.tabs()))


def test_finder_dismissal_by_mouse():
    """A modal owns the pointer. Clicking past it must not also land on the
    layout behind -- that is how you focus the pane you were clicking away
    from."""
    with Session(SH, cols=70, rows=16) as s:
        a, b = setup_panes(s)
        s.api("focus", id=b)
        s.settle()

        s.key("f")
        s.settle(80)
        close = [h for h in s.snapshot().hits if h["action"] == "closefind"]
        check("it has a close button, like any modal", len(close) == 1,
              str(s.snapshot().hits))
        if close:
            s.click(close[0]["x"], close[0]["y"])
            s.settle(80)
            check("which closes it", not is_open(s))
            check("without changing the focus", s.focused()["id"] == b,
                  str(s.focused()))

        s.key("f")
        s.settle(80)
        check("it is open again", is_open(s))
        s.click(1, 15)  # the corner of the screen, well outside the box
        s.settle(80)
        check("a click outside dismisses it", not is_open(s))
        check("and does not reach the pane behind it",
              s.focused()["id"] == b, str(s.focused()))


def test_finder_marks_where_you_are():
    with Session(SH, cols=70, rows=16) as s:
        a, b = setup_panes(s)
        s.api("focus", id=b)
        s.settle()
        s.key("f")
        s.settle(80)
        marked = [r for r in rows(s) if "\u2022" in r[2]]
        check("exactly one row is marked as the current pane",
              len(marked) == 1, str(rows(s)))
        if marked:
            check("and it is the pane we are in", marked[0][0] == f"find:{b}",
                  str(marked))


def test_finder_over_a_collapsed_layout():
    """The finder is how you reach a pane a small screen has collapsed away."""
    with Session(SH, cols=120, rows=32) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        s.key("\\\\")
        s.settle()
        target = s.panes()[0]["id"]
        s.api("set-purpose", target="pane", id=target, purpose="agent:findme",
              declared=True)
        s.api("focus", id=s.panes()[-1]["id"])
        s.resize(50, 14)
        s.settle()
        check("the target is collapsed away",
              [p for p in s.panes() if p["id"] == target][0]["hidden"])

        s.key("f")
        s.send("findme")
        s.settle(80)
        s.send(r"\r")
        s.settle(80)
        check("the finder reaches a collapsed pane and expands it",
              not [p for p in s.panes() if p["id"] == target][0]["hidden"],
              str(s.panes()))


if __name__ == "__main__":
    test_status_bar()
    test_finder_keyboard()
    test_finder_navigation()
    test_finder_query_editing()
    test_finder_mouse()
    test_finder_dismissal_by_mouse()
    test_finder_marks_where_you_are()
    test_finder_over_a_collapsed_layout()
    sys.exit(report())
