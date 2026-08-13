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
        check("the bar shows a pane count", "1 panes" in bar, repr(bar))
        check("the bar shows a new-tab button", "+tab" in bar, repr(bar))

        x = bar.index("+tab")
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


def test_finder_keyboard():
    with Session(SH, cols=70, rows=16) as s:
        a, b = setup_panes(s)

        s.key("f")
        s.settle(80)
        snap = s.snapshot()
        check("the finder opens", "find:" in snap.screen(), repr(snap.screen()[:200]))
        check("it lists panes from every tab",
              "agent:main" in snap.screen() and "service:web" in snap.screen(),
              repr(snap.screen()))

        s.send("web")
        s.settle(80)
        snap = s.snapshot()
        check("typing filters the list", "service:web" in snap.screen()
              and "agent:main" not in snap.screen(), repr(snap.screen()))

        s.send(r"\r")
        s.settle(80)
        check("enter focuses the match", s.focused()["id"] == b,
              str(s.focused()))
        check("and closes the finder", "find:" not in s.snapshot().screen())

    with Session(SH, cols=70, rows=16) as s:
        setup_panes(s)
        s.key("f")
        s.send("zzzz")
        s.settle(80)
        check("a query matching nothing is not fatal",
              "find: zzzz" in s.snapshot().screen(), repr(s.snapshot().screen()[:160]))
        s.send(r"\x7f\x7f\x7f\x7f")  # backspace back to empty
        s.settle(80)
        check("backspace restores the list", "agent:main" in s.snapshot().screen(),
              repr(s.snapshot().screen()[:200]))
        s.send(r"\e")
        s.settle(120)
        check("escape closes it", "find:" not in s.snapshot().screen())
        check("and the session is unharmed", s.api("alive")["alive"])


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
    test_finder_mouse()
    test_finder_over_a_collapsed_layout()
    sys.exit(report())
