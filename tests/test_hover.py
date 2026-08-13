#!/usr/bin/env python3
"""Focus follows the mouse — and the guards that make that liveable.

The feature is three lines. The reason it is not obnoxious is the guards: it
must not steal focus mid-chord, reach past an open finder, interrupt a drag,
or expand a collapsed pane just because the pointer crossed its header.
"""
import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def hover(s, x, y):
    """Motion with no button held: SGR button 35 = release/none + motion."""
    s.send(rf"\e[<35;{x + 1};{y + 1}M")


def focused_id(s):
    f = s.focused()
    return f["id"] if f else None


def test_hover_focuses():
    with Session(SH, cols=70, rows=14) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        check("the new pane has focus", focused_id(s) == right["id"])

        hover(s, left["content_x"] + 2, left["content_y"] + 1)
        s.settle(60)
        check("hovering a pane focuses it", focused_id(s) == left["id"],
              str(s.panes()))

        hover(s, right["content_x"] + 2, right["content_y"] + 1)
        s.settle(60)
        check("and back", focused_id(s) == right["id"])

        hover(s, left["x"] + 4, left["y"])  # the title row
        s.settle(60)
        check("hovering a title bar focuses it too", focused_id(s) == left["id"])

        hover(s, 0, 0)  # the gap ring, which belongs to nothing
        s.settle(60)
        check("hovering nothing changes nothing", focused_id(s) == left["id"])


def test_guards():
    with Session(SH, cols=70, rows=14) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()

        # mid-chord: the prefix is held, the next key is a command
        s.send(r"\x01")
        hover(s, left["content_x"] + 2, left["content_y"] + 1)
        s.settle(60)
        check("hover does not steal focus mid-chord",
              focused_id(s) == right["id"], str(s.panes()))
        s.send("g")  # release the prefix harmlessly

        # with the finder open, the pane underneath must not take focus
        s.key("f")
        s.settle(60)
        hover(s, left["content_x"] + 2, left["content_y"] + 1)
        s.settle(60)
        check("hover does not reach past the finder",
              focused_id(s) == right["id"], str(s.panes()))
        s.send(r"\e")
        s.settle(60)

        # during a drag, the mouse belongs to the drag
        s.send(rf"\e[<0;{right['x'] + 5};{right['y'] + 1}M")   # press a title
        hover(s, left["content_x"] + 2, left["content_y"] + 1)  # (button held)
        s.settle(60)
        check("a drag owns the mouse", focused_id(s) == right["id"],
              str(s.panes()))
        s.send(r"\x01g")  # any key ends the drag


def test_hover_does_not_expand_collapsed_panes():
    with Session(SH, cols=120, rows=30) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        s.key("\\\\")
        s.settle()
        s.resize(50, 14)
        s.settle()
        hidden = [p for p in s.panes() if p["hidden"]]
        check("something is collapsed", hidden != [], str(s.panes()))
        if not hidden:
            return
        h = hidden[0]
        before = focused_id(s)
        hover(s, h["x"] + 3, h["y"])
        s.settle(60)
        check("hovering a collapsed header does not expand it",
              [p for p in s.panes() if p["id"] == h["id"]][0]["hidden"],
              str(s.panes()))
        check("nor move focus", focused_id(s) == before, str(s.panes()))
        # clicking it still does
        s.click(h["x"] + 3, h["y"])
        s.settle()
        check("clicking it still expands it",
              not [p for p in s.panes() if p["id"] == h["id"]][0]["hidden"])


def test_can_be_turned_off():
    path = cfg("focus_follows_mouse false\n")
    with Session(SH, cols=70, rows=14, config=path) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        hover(s, left["content_x"] + 2, left["content_y"] + 1)
        s.settle(60)
        check("with the option off, hovering does nothing",
              focused_id(s) == right["id"], str(s.panes()))
        s.click(left["content_x"] + 2, left["content_y"] + 1)
        s.settle(60)
        check("but clicking still focuses", focused_id(s) == left["id"],
              str(s.panes()))
    os.unlink(path)


def test_panes_still_get_motion():
    """?1003 also fixes something: a pane that tracks hover can now see it."""
    tracker = ["/bin/sh", "-c",
               'stty raw -echo; printf "\\033[?1003h\\033[?1006h"; cat -v']
    with Session(tracker, cols=70, rows=12) as s:
        s.settle()
        p = s.pane()
        hover(s, p["content_x"] + 4, p["content_y"] + 2)
        s.settle()
        out = s.snapshot().pane_text(p)
        check("motion reaches a pane that asked for any-event tracking",
              "^[[<35;5;3M" in out, repr(out[:120]))

    quiet = ["/bin/sh", "-c",
             'stty raw -echo; printf "\\033[?1000h\\033[?1006h"; cat -v']
    with Session(quiet, cols=70, rows=12) as s:
        s.settle()
        p = s.pane()
        hover(s, p["content_x"] + 4, p["content_y"] + 2)
        s.settle()
        out = s.snapshot().pane_text(p)
        check("but not one that only asked for clicks", "[<" not in out,
              repr(out[:120]))


if __name__ == "__main__":
    test_hover_focuses()
    test_guards()
    test_hover_does_not_expand_collapsed_panes()
    test_can_be_turned_off()
    test_panes_still_get_motion()
    sys.exit(report())
