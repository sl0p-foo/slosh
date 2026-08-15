#!/usr/bin/env python3
"""The command palette: every action by name, without knowing its key.

The cheatsheet answers "what is the key for this?" and then needs you to press
it. This answers "just do the thing", which is the question you have when you
cannot remember there was a key at all -- and it is the only way to reach an
action that nothing is bound to.

It shares its box, its keys and its mouse handling with the pane finder (they
are one picker with a subject), so the navigation and editing checks live in
test_finder.py and are not repeated here.
"""
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;shell\\007"; stty raw -echo; cat']

OPEN = r"\x01p"   # the default bind: prefix, then p


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def is_open(s):
    return any(h["action"].startswith("run:") or h["action"] == "closepalette"
               for h in s.snapshot().hits)


def counter(s):
    import re
    for line in s.snapshot().screen().split("\n"):
        m = re.search(r"(\d+) of (\d+)", line)
        if m:
            return int(m.group(1)), int(m.group(2))
    return None


def rows(s):
    snap = s.snapshot()
    return [snap.text[h["y"]] for h in
            sorted((h for h in snap.hits if h["action"].startswith("run:")),
                   key=lambda h: h["y"])]


def test_it_opens_and_lists_commands():
    with Session(SH, cols=80, rows=22) as s:
        s.settle()
        s.send(OPEN)
        s.settle(100)
        check("the palette opens", is_open(s), repr(s.snapshot().screen()[:200]))
        screen = s.snapshot().screen()
        check("it is titled as commands, not as the finder",
              "commands" in screen, repr(screen[:300]))
        check("it lists actions by their phrase",
              "split into columns" in screen, repr(screen))
        check("with the chord that would have run them",
              any("split into columns" in r and "\\" in r for r in rows(s)),
              str(rows(s)))
        check("and there are a lot of them", (counter(s) or (0, 0))[1] > 20,
              str(counter(s)))


def test_it_lists_actions_that_have_no_key():
    """The palette's reason to exist over the cheatsheet, which can only show
    what is bound. Nothing binds scroll-up/scroll-down by default."""
    with Session(SH, cols=80, rows=22) as s:
        s.settle()
        s.send(OPEN)
        s.send("a line")
        s.settle(100)
        listed = [r for r in rows(s) if "up a line" in r]
        check("an unbound action is listed", listed != [], str(rows(s)))
        if listed:
            check("with no chord against it, rather than a made-up one",
                  "C-a" not in listed[0], repr(listed[0]))


def test_typing_filters_it():
    with Session(SH, cols=80, rows=22) as s:
        s.settle()
        s.send(OPEN)
        s.settle(80)
        everything = counter(s)[1]

        s.send("columns")
        s.settle(80)
        check("the phrase finds it", counter(s) == (1, 1), str(counter(s)))

        s.send(r"\x15split-cols")   # C-u, then the name a config file uses
        s.settle(80)
        check("so does the name you would write in a config",
              counter(s) == (1, 1), str(counter(s)))

        s.send(r"\x15scroll")
        s.settle(80)
        c = counter(s)
        check("and so does a group", c and 1 < c[1] < everything, str(c))

        s.send(r"\x15zzzz")
        s.settle(80)
        check("a query matching nothing says so",
              "no command matches" in s.snapshot().screen(),
              repr(s.snapshot().screen()[:400]))


def test_enter_runs_the_command():
    with Session(SH, cols=80, rows=22) as s:
        s.settle()
        check("one pane to start with", len(s.panes()) == 1)
        s.send(OPEN)
        s.send("columns")
        s.settle(80)
        s.send(r"\r")
        s.settle(150)
        check("the command ran", len(s.panes()) == 2, str(s.panes()))
        check("and the palette closed behind it", not is_open(s))


def test_clicking_a_row_runs_it():
    with Session(SH, cols=80, rows=22) as s:
        s.settle()
        s.send(OPEN)
        s.send("columns")
        s.settle(80)
        row = [h for h in s.snapshot().hits if h["action"].startswith("run:")]
        check("the row is in the hit list", len(row) == 1, str(s.snapshot().hits))
        if not row:
            return
        s.click(row[0]["x"] + 2, row[0]["y"])
        s.settle(150)
        check("clicking it runs it", len(s.panes()) == 2, str(s.panes()))
        check("and closes the palette", not is_open(s))


def test_it_closes_the_ways_a_modal_does():
    with Session(SH, cols=80, rows=22) as s:
        s.settle()
        s.send(OPEN)
        s.settle(80)
        s.send(r"\e")
        s.settle(80)
        check("escape closes it", not is_open(s))

        s.send(OPEN)
        s.settle(80)
        close = [h for h in s.snapshot().hits if h["action"] == "closepalette"]
        check("it has a close button", len(close) == 1, str(s.snapshot().hits))
        if close:
            s.click(close[0]["x"], close[0]["y"])
            s.settle(80)
            check("which closes it", not is_open(s))

        s.send(OPEN)
        s.settle(80)
        s.click(1, 20)
        s.settle(80)
        check("and a click outside dismisses it", not is_open(s))
        check("without running anything", len(s.panes()) == 1, str(s.panes()))


def test_it_does_not_offer_typing_the_prefix():
    """`literal-prefix` means "send this key to the pane", which is not a
    command you can ask for by name -- picked from a list it would type into
    a pane hidden behind that list."""
    with Session(SH, cols=80, rows=22) as s:
        s.settle()
        s.send(OPEN)
        s.send("prefix")
        s.settle(100)
        check("the prefix is not on the menu",
              "no command matches" in s.snapshot().screen(),
              repr(s.snapshot().screen()[:400]))


def test_the_bind_is_configurable():
    conf = cfg("""
        keys {
            prefix "ctrl+a"
            bind "space" "palette"
        }
    """)
    with Session(SH, cols=80, rows=22, config=conf) as s:
        s.settle()
        s.send(r"\x01 ")     # prefix, then space
        s.settle(100)
        check("the palette opens on the key it was bound to", is_open(s))
        s.send("run a command")   # its own row is well down a list of thirty
        s.settle(80)
        check("and says so on its own row",
              any("run a command" in r and "space" in r for r in rows(s)),
              str([r for r in rows(s) if "run a command" in r]))


def test_the_finder_is_still_its_own_thing():
    """They share a box; they must not share a list."""
    with Session(SH, cols=80, rows=22) as s:
        s.settle()
        s.send(r"\x01f")
        s.settle(100)
        check("the finder lists panes, not commands",
              not is_open(s) and "split into columns" not in s.snapshot().screen(),
              repr(s.snapshot().screen()[:300]))
        s.send(r"\e")
        s.settle(80)
        s.send(OPEN)
        s.settle(100)
        check("and the palette lists commands", is_open(s))


if __name__ == "__main__":
    test_it_opens_and_lists_commands()
    test_it_lists_actions_that_have_no_key()
    test_typing_filters_it()
    test_enter_runs_the_command()
    test_clicking_a_row_runs_it()
    test_it_closes_the_ways_a_modal_does()
    test_it_does_not_offer_typing_the_prefix()
    test_the_bind_is_configurable()
    test_the_finder_is_still_its_own_thing()
    sys.exit(report())
