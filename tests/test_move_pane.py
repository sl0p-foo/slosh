#!/usr/bin/env python3
"""Moving a pane from one tab to another.

The mechanism, not the presentation: there is no key for this yet, deliberately.
What matters here is that the tree surgery is sound and that **the program in the
pane never notices** -- a move is the same pty in a different tab, not a new pane
somewhere else and a funeral here. Every check below is about one of those two.
"""
import os
import sys
import tempfile
import time

from harness import Session, check, report

# A pane that says something, then echoes what it is sent: enough to prove the same
# program is still running on the other side of a move.
ECHO = ["/bin/sh", "-c", 'printf "alive\\n"; stty raw -echo; exec cat']


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def by_tab(s):
    """Which panes are in which tab, by tab **id**.

    `tab_id`, not `tab`: the latter is where the tab sits in the strip, and the two
    stop agreeing the moment a tab is removed -- which is half of what this file is
    about."""
    out = {t["id"]: [] for t in s.tabs()}
    for p in s.panes():
        out.setdefault(p["tab_id"], []).append(p["id"])
    return {k: sorted(v) for k, v in out.items()}


def two_tabs(s):
    """Two panes in the first tab, one in a second. Returns (tab ids, pane ids)."""
    s.settle()
    s.key("\\\\")          # split: two panes here
    s.settle(40)
    s.key("c")            # a second tab, with one
    s.settle(40)
    tabs = [t["id"] for t in s.tabs()]
    return tabs


def test_a_pane_moves_and_keeps_running():
    """The whole point. The pane prints before the move and answers after it, so the
    thing on the other side is the same program and not a fresh one."""
    with Session(ECHO, cols=90, rows=16) as s:
        tabs = two_tabs(s)
        s.until_text("alive")
        before = by_tab(s)
        mover = before[tabs[0]][1]
        check("two tabs to start with", len(tabs) == 2, str(before))

        reply = s.api("move-pane", id=mover, tab=tabs[1])
        s.settle(60)
        after = by_tab(s)
        check("the move is accepted", reply.get("ok") is True, str(reply))
        check("the pane is in the other tab now",
              mover in after[tabs[1]] and mover not in after[tabs[0]], str(after))
        check("and no pane was created or lost",
              sum(len(v) for v in after.values()) == sum(len(v) for v in before.values()),
              "%s -> %s" % (before, after))

        # Talk to it where it landed: same pty, same shell, still reading.
        s.api("select-tab", id=tabs[1])
        s.settle(40)
        s.api("focus", id=mover)
        s.raw("still-here\\n")
        snap = s.until_text("still-here")
        pane = [p for p in s.panes() if p["id"] == mover][0]
        check("the program in it is the one that was there before",
              "still-here" in snap.pane_text(pane), repr(snap.pane_text(pane)[:120]))
        check("and it is alive", pane["alive"] is True, str(pane))


def test_it_lands_beside_the_destination_focus():
    """Beside what that tab was looking at, and focused -- you moved it there on
    purpose. `dir` says beside or under, as it does for a split."""
    with Session(ECHO, cols=90, rows=16) as s:
        tabs = two_tabs(s)
        s.until_text("alive")
        mover = by_tab(s)[tabs[0]][1]
        host = by_tab(s)[tabs[1]][0]

        s.api("move-pane", id=mover, tab=tabs[1], dir="cols")
        s.api("select-tab", id=tabs[1])
        s.settle(60)
        rects = {p["id"]: (p["x"], p["y"], p["w"], p["h"]) for p in s.panes()
                 if p["tab_id"] == tabs[1]}
        check("both panes are in the destination", len(rects) == 2, str(rects))
        check("side by side, the arrival on the right",
              rects[mover][0] > rects[host][0], str(rects))
        check("and the arrival has the focus",
              [p["focused"] for p in s.panes() if p["id"] == mover] == [True],
              str([p for p in s.panes() if p["id"] == mover]))

        s.api("move-pane", id=mover, tab=tabs[0], dir="rows")
        s.api("select-tab", id=tabs[0])
        s.settle(60)
        rects = {p["id"]: (p["x"], p["y"], p["w"], p["h"]) for p in s.panes()
                 if p["tab_id"] == tabs[0]}
        check("`rows` puts it underneath",
              rects[mover][1] > min(r[1] for r in rects.values()), str(rects))


def test_emptying_a_tab_removes_it():
    """A tab whose last pane leaves has nothing to be, and the destination must
    still be the tab that was asked for -- which is why the argument is an id.
    Naming it by index would quietly mean a different tab once the removal shifted
    everything after it."""
    with Session(ECHO, cols=90, rows=16) as s:
        tabs = two_tabs(s)
        s.until_text("alive")
        lonely = by_tab(s)[tabs[1]][0]      # the only pane of the *second* tab

        reply = s.api("move-pane", id=lonely, tab=tabs[0])
        s.settle(60)
        after = by_tab(s)
        check("the move worked", reply.get("ok") is True, str(reply))
        check("the tab it left is gone", tabs[1] not in [t["id"] for t in s.tabs()],
              str([t["id"] for t in s.tabs()]))
        check("and everything is in the one that is left",
              sorted(after[tabs[0]]) == sorted(sum(after.values(), [])), str(after))
        check("three panes, none lost", len(after[tabs[0]]) == 3, str(after))


def test_a_pane_can_be_moved_into_a_tab_of_its_own():
    with Session(ECHO, cols=90, rows=16) as s:
        tabs = two_tabs(s)
        s.until_text("alive")
        mover = by_tab(s)[tabs[0]][1]
        watching = [t["id"] for t in s.tabs() if t.get("active")]

        reply = s.api("move-pane", id=mover, tab=0, name="solo")
        s.settle(60)
        made = reply.get("tab")
        check("it answers with the tab it made", reply.get("ok") and made, str(reply))
        check("the pane is the only thing in it", by_tab(s).get(made) == [mover],
              str(by_tab(s)))
        check("and the tab has the name it was given",
              [t["name"] for t in s.tabs() if t["id"] == made] == ["solo"],
              str(s.tabs()))
        # A move does not follow the pane: which tab you are looking at is a
        # question about intent, and this is the mechanism.
        check("we are still looking at the tab we were looking at",
              [t["id"] for t in s.tabs() if t.get("active")] == watching,
              str([t["id"] for t in s.tabs() if t.get("active")]))


def test_the_refusals():
    """Nothing to do, or nowhere to do it. Each says no rather than half-doing it."""
    with Session(ECHO, cols=90, rows=16) as s:
        tabs = two_tabs(s)
        s.until_text("alive")
        here = by_tab(s)[tabs[0]][0]
        lonely = by_tab(s)[tabs[1]][0]

        check("no such pane", s.api("move-pane", id=999, tab=tabs[1]).get("ok")
              is False, "")
        check("no such tab", s.api("move-pane", id=here, tab=999).get("ok")
              is False, "")
        check("already in that tab",
              s.api("move-pane", id=here, tab=tabs[0]).get("ok") is False, "")
        # A pane alone in its tab is already in a tab of its own; making it another
        # one would remove the tab it is in and add an identical one.
        check("a pane alone in its tab has nowhere of its own to go",
              s.api("move-pane", id=lonely, tab=0).get("ok") is False, "")
        check("and none of that moved anything",
              by_tab(s) == {tabs[0]: sorted(by_tab(s)[tabs[0]]),
                            tabs[1]: [lonely]}, str(by_tab(s)))


def test_a_move_drops_what_the_old_tab_thought():
    """A zoom names one of a tab's panes and a minimised pane is one that tab put
    away. Carried across, the first would zoom a pane that has left and the second
    would file the arrival in a strip nobody asked for."""
    with Session(ECHO, cols=90, rows=16) as s:
        tabs = two_tabs(s)
        s.until_text("alive")
        mover = by_tab(s)[tabs[0]][1]

        s.api("focus", id=mover)
        s.key("m")                      # minimise it
        s.settle(40)
        check("it is put away", [p for p in s.panes() if p["id"] == mover][0]["h"] <= 1,
              str([p for p in s.panes() if p["id"] == mover]))

        s.api("move-pane", id=mover, tab=tabs[1])
        s.api("select-tab", id=tabs[1])
        s.settle(60)
        pane = [p for p in s.panes() if p["id"] == mover][0]
        check("it arrives laid out, not filed away", pane["h"] > 1, str(pane))
        check("and the tab it left is not zoomed onto a pane that has gone",
              all(p["h"] > 1 for p in s.panes() if p["tab_id"] == tabs[0]),
              str([p for p in s.panes() if p["tab_id"] == tabs[0]]))


def test_the_layout_survives_a_move():
    """A tree that has been cut and grafted has to be a tree: no overlaps, nothing
    off screen, and it dumps and reloads as itself."""
    with Session(ECHO, cols=100, rows=20) as s:
        tabs = two_tabs(s)
        s.until_text("alive")
        s.key("-")                       # a third pane in the first tab
        s.settle(40)

        for target in (tabs[1], tabs[0], tabs[1]):
            movable = [p["id"] for p in s.panes() if p["tab_id"] != target]
            if not movable:
                continue
            s.api("move-pane", id=movable[0], tab=target)
            s.settle(60)

        rs = [(p["x"], p["y"], p["w"], p["h"]) for p in s.panes()
              if p["tab_id"] == [t["id"] for t in s.tabs() if t.get("active")][0]]
        for i in range(len(rs)):
            for j in range(i + 1, len(rs)):
                ax, ay, aw, ah = rs[i]
                bx, by, bw, bh = rs[j]
                if ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah:
                    check("panes do not overlap after moving", False,
                          "%s vs %s" % (rs[i], rs[j]))
                    return
        check("the visible tab is a sane layout after three moves", True, str(rs))

        dump = s.api("dump-layout").get("kdl", "")
        check("and it dumps as a layout", "layout {" in dump and "pane" in dump,
              repr(dump[:120]))
        path = cfg(dump)
        reply = s.api("apply-layout", path=path, replace=True)
        s.settle(60)
        check("that the session can be rebuilt from", reply.get("ok") is True,
              str(reply))
        os.unlink(path)


def test_the_keys_push_a_pane_a_tab_along():
    """`C-a >` and `C-a <`, and a word about where it went -- you do not follow it,
    and a key that might have done nothing is a key you press again."""
    with Session(ECHO, cols=90, rows=16) as s:
        tabs = two_tabs(s)
        s.until_text("alive")
        s.api("select-tab", id=tabs[0])
        s.settle(40)
        watching = [t["id"] for t in s.tabs() if t.get("active")]
        moved = [p["id"] for p in s.panes()
                 if p["tab_id"] == tabs[0] and p["focused"]][0]

        s.key(">")
        s.settle(60)
        check("`C-a >` moves the focused pane to the next tab",
              moved in by_tab(s)[tabs[1]], str(by_tab(s)))
        check("and says where it went", "moved to tab" in s.snapshot().screen(),
              repr(s.snapshot().screen()[-120:]))
        check("without following it",
              [t["id"] for t in s.tabs() if t.get("active")] == watching,
              str([t["id"] for t in s.tabs() if t.get("active")]))

        s.api("select-tab", id=tabs[1])
        s.api("focus", id=moved)
        s.settle(40)
        s.key("<")
        s.settle(60)
        check("`C-a <` sends it back the other way",
              moved in by_tab(s)[tabs[0]], str(by_tab(s)))


def test_the_key_for_a_tab_of_its_own():
    with Session(ECHO, cols=90, rows=16) as s:
        tabs = two_tabs(s)
        s.until_text("alive")
        s.api("select-tab", id=tabs[0])
        s.settle(40)
        moved = [p["id"] for p in s.panes()
                 if p["tab_id"] == tabs[0] and p["focused"]][0]

        s.key("b")
        s.settle(60)
        made = [t["id"] for t in s.tabs() if t["id"] not in tabs]
        check("`C-a b` breaks it out into a new tab", len(made) == 1, str(s.tabs()))
        check("with the pane in it", by_tab(s).get(made[0]) == [moved],
              str(by_tab(s)))
        check("and says so", "tab of its own" in s.snapshot().screen(),
              repr(s.snapshot().screen()[-120:]))


def test_one_tab_has_nowhere_to_push_to():
    """Refused with a word rather than silently: the only tab is the one it is in."""
    with Session(ECHO, cols=80, rows=14) as s:
        s.settle()
        s.key("\\\\")
        s.settle(40)
        before = by_tab(s)
        s.key(">")
        s.settle(60)
        check("nothing moved", by_tab(s) == before, "%s -> %s" % (before, by_tab(s)))
        check("and it says why", "only one tab" in s.snapshot().screen(),
              repr(s.snapshot().screen()[-120:]))


def test_pushing_the_last_pane_out_takes_the_tab_with_it():
    """The tab you were looking at can be the one that goes. There is nowhere to
    stay, so you arrive in the tab that survived -- and the toast reads the
    destination's number *after* the removal, which is the number on screen."""
    with Session(ECHO, cols=90, rows=16) as s:
        tabs = two_tabs(s)
        s.until_text("alive")
        s.api("select-tab", id=tabs[1])   # the tab with one pane in it
        s.settle(40)
        s.key(">")
        s.settle(60)
        check("the tab it emptied is gone",
              [t["id"] for t in s.tabs()] == [tabs[0]], str(s.tabs()))
        check("all three panes are in the survivor",
              len(by_tab(s)[tabs[0]]) == 3, str(by_tab(s)))
        check("and that is what we are looking at",
              [t["id"] for t in s.tabs() if t.get("active")] == [tabs[0]],
              str(s.tabs()))


def test_the_sheet_and_the_palette_know_about_them():
    """The palette comes free with an action, which is the reason to make these
    actions rather than special cases in the key handler. And the cheatsheet has to
    print `>`, not the bare period it also answers to -- what you would type."""
    with Session(ECHO, cols=100, rows=30) as s:
        s.settle()
        s.key("?")
        s.settle(60)
        sheet = s.snapshot().screen()
        check("the sheet lists pushing a pane along", "tab after" in sheet,
              repr([l.strip() for l in s.snapshot().text if "tab after" in l]))
        check("under the key you would type", ">" in sheet, "")
        s.send("\\x1b")
        s.settle(40)

        s.key("p")
        s.settle(40)
        s.send("tab of its own")
        s.settle(60)
        pal = s.snapshot().screen()
        check("and the palette finds it by its label", "tab of its own" in pal,
              repr([l.strip() for l in s.snapshot().text if "own" in l]))


if __name__ == "__main__":
    test_a_pane_moves_and_keeps_running()
    test_it_lands_beside_the_destination_focus()
    test_emptying_a_tab_removes_it()
    test_a_pane_can_be_moved_into_a_tab_of_its_own()
    test_the_refusals()
    test_a_move_drops_what_the_old_tab_thought()
    test_the_layout_survives_a_move()
    test_the_keys_push_a_pane_a_tab_along()
    test_the_key_for_a_tab_of_its_own()
    test_one_tab_has_nowhere_to_push_to()
    test_pushing_the_last_pane_out_takes_the_tab_with_it()
    test_the_sheet_and_the_palette_know_about_them()
    sys.exit(report())
