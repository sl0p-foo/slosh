#!/usr/bin/env python3
"""The pane title is a name, not an edge.

The top row is the frame's drag handle *and* its top edge: resting on it arms
a split guide and clicking it splits upward. That is right for the border, and
wrong for the cells the title is painted on — a double-click there is meant to
rename the pane, and it would split twice on its way to the rename.

So the title text carves its own region out of the top row. It still drags and
still focuses; it just no longer suggests or performs a split. Everything the
rest of the top row does must keep working, which is most of what is checked
here.
"""

import sys
import time

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;shell\\007"; stty raw -echo; cat']

HEAVY = "\u2501"  # the armed top edge of the split guide
CURSOR = "\u2588"  # the rename editor's caret


def hover(s, x, y):
    s.send(rf"\e[<35;{x + 1};{y + 1}M")


def rest(s, x, y):
    """The guide arms on dwell (250ms), not on contact."""
    hover(s, x, y)
    time.sleep(0.35)


def click(s, x, y):
    s.send(rf"\e[<0;{x + 1};{y + 1}M")
    s.send(rf"\e[<0;{x + 1};{y + 1}m")


def title_span(snap, pane):
    """(first, last, middle) columns of the title text on the pane's top row."""
    row = snap.line(pane["y"])
    x = row.find(" shell ")
    assert x >= 0, f"no title in top row: {row!r}"
    return x, x + 6, x + 3


def off_title(pane):
    """A top-row cell the centered title cannot reach."""
    return pane["x"] + 2


def col_of(snap, pane, text):
    """Middle column of `text` on the pane's top row."""
    row = snap.line(pane["y"])
    x = row.find(text)
    assert x >= 0, f"{text!r} not in {row!r}"
    return x + len(text) // 2


def dbl(s, x, y):
    s.click(x, y)
    s.click(x, y)


def open_editor(s, pane, text="shell"):
    dbl(s, col_of(s.snapshot(), pane, text), pane["y"])
    s.settle(80)


def backspace(s, n):
    for _ in range(n):
        s.send(r"\x7f")


def titles(s):
    return [p["title"] for p in s.panes()]


def test_the_title_text_is_its_own_region():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        snap = s.snapshot()
        first, last, mid = title_span(snap, p)

        check(
            "the title text hit-tests as the pane's name",
            snap.hit_at(mid, p["y"]) == f"panetitle:{p['id']}",
            str(snap.hit_at(mid, p["y"])),
        )
        check(
            "both its ends do too",
            snap.hit_at(first, p["y"]) == f"panetitle:{p['id']}"
            and snap.hit_at(last, p["y"]) == f"panetitle:{p['id']}",
            f"{snap.hit_at(first, p['y'])} .. {snap.hit_at(last, p['y'])}",
        )
        check(
            "the cell just outside it is still the top edge",
            snap.hit_at(first - 1, p["y"]) == f"title:{p['id']}",
            str(snap.hit_at(first - 1, p["y"])),
        )
        check(
            "and so is the rest of the top row",
            snap.hit_at(off_title(p), p["y"]) == f"title:{p['id']}",
            str(snap.hit_at(off_title(p), p["y"])),
        )


def test_resting_on_the_title_arms_no_guide():
    with Session(SH, cols=60, rows=26) as s:
        s.settle()
        p = s.pane()
        _, _, mid = title_span(s.snapshot(), p)

        rest(s, mid, p["y"])
        s.settle(60)
        check(
            "resting on the title suggests no split",
            HEAVY not in s.snapshot().line(p["y"]),
            repr(s.snapshot().line(p["y"])),
        )

        # The control: the same dwell two cells away must still arm it, or the
        # check above would pass simply because the guide is broken.
        rest(s, off_title(p), p["y"])
        s.settle(60)
        check(
            "resting on the top edge still does",
            HEAVY in s.snapshot().line(p["y"]),
            repr(s.snapshot().line(p["y"])),
        )


def test_clicking_the_title_does_not_split():
    with Session(SH, cols=60, rows=26) as s:
        s.settle()
        p = s.pane()
        _, _, mid = title_span(s.snapshot(), p)

        click(s, mid, p["y"])
        s.settle(80)
        check(
            "clicking the title splits nothing",
            len(s.panes()) == 1,
            str(len(s.panes())),
        )

        click(s, off_title(p), p["y"])
        s.settle(80)
        check(
            "clicking the top edge still splits up",
            len(s.panes()) == 2,
            str(len(s.panes())),
        )


def test_double_clicking_the_title_does_not_split():
    """The gesture the exclusion exists for: it must survive both halves."""
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        _, _, mid = title_span(s.snapshot(), p)

        click(s, mid, p["y"])
        click(s, mid, p["y"])
        s.settle(80)
        check(
            "a double-click on the title leaves one pane",
            len(s.panes()) == 1,
            str(len(s.panes())),
        )


def test_the_title_still_drags_the_pane():
    with Session(SH, cols=70, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        _, _, mid = title_span(s.snapshot(), left)

        # press on the left pane's *title text*, move onto the right pane
        s.send(rf"\e[<0;{mid + 1};{left['y'] + 1}M")
        s.send(rf"\e[<32;{right['x'] + 5};{right['y'] + 3}M")
        s.settle(60)
        s.send(rf"\e[<0;{right['x'] + 5};{right['y'] + 3}m")
        s.settle()

        panes = {p["id"]: p for p in s.panes()}
        check(
            "dragging by the title still moves the pane",
            len(panes) == 2 and panes[left["id"]]["x"] > panes[right["id"]]["x"],
            str([(p["id"], p["x"]) for p in s.panes()]),
        )


def test_hovering_the_title_still_focuses():
    with Session(SH, cols=70, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        focused = s.focused()["id"]
        other = left if focused == right["id"] else right
        _, _, mid = title_span(s.snapshot(), other)

        hover(s, mid, other["y"])
        s.settle(60)
        check(
            "hovering the title still focuses its pane",
            s.focused()["id"] == other["id"],
            f"{s.focused()['id']} != {other['id']}",
        )


def test_double_click_opens_an_editor_seeded_with_the_name():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        open_editor(s, p)
        row = s.snapshot().line(p["y"])
        check("double-clicking the title opens an editor", CURSOR in row, repr(row))
        check(
            "seeded with the current name, so a rename is an edit",
            "shell" + CURSOR in row,
            repr(row),
        )


def test_enter_keeps_the_new_name():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        open_editor(s, p)
        backspace(s, 5)
        s.send("api")
        s.send(r"\r")
        s.settle(80)
        check("Enter commits the new name", titles(s) == ["api"], str(titles(s)))
        check(
            "and the editor is gone",
            CURSOR not in s.snapshot().line(p["y"]),
            repr(s.snapshot().line(p["y"])),
        )


def test_escape_abandons_it():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        open_editor(s, p)
        backspace(s, 5)
        s.send("discarded")
        s.send(r"\e")
        s.settle(80)
        check(
            "Escape leaves the old name alone", titles(s) == ["shell"], str(titles(s))
        )
        check(
            "and closes the editor",
            CURSOR not in s.snapshot().line(p["y"]),
            repr(s.snapshot().line(p["y"])),
        )


def test_clicking_away_keeps_the_name():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        open_editor(s, p)
        backspace(s, 5)
        s.send("elsewhere")
        s.click(p["content_x"] + 2, p["content_y"] + 1)
        s.settle(80)
        check(
            "clicking away commits, the way leaving a field does",
            titles(s) == ["elsewhere"],
            str(titles(s)),
        )


def test_a_typed_name_outranks_the_program():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        open_editor(s, p)
        backspace(s, 5)
        s.send("mine")
        s.send(r"\r")
        s.settle(80)

        s.raw(r"\e]2;prog-later\x07")
        s.settle(120)
        check(
            "the program retitling itself does not undo a rename",
            titles(s) == ["mine"],
            str(titles(s)),
        )

        # Clearing the name hands the pane back — to what the program calls
        # itself *now*, not what it said when the rename began.
        open_editor(s, p, "mine")
        backspace(s, 8)
        s.send(r"\r")
        s.settle(80)
        check(
            "an empty name returns the pane to its program's title",
            titles(s) == ["prog-later"],
            str(titles(s)),
        )


def test_a_script_can_name_a_pane_too():
    """The gesture is not the only way in. A program that keeps setting a title it
    stopped meaning -- an agent still spinning the summary of a finished task -- is
    overruled by naming the pane, and that has to be reachable from tooling, not
    only from a double-click on somebody's screen."""
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        check(
            "the program's title to start with", titles(s) == ["shell"], str(titles(s))
        )

        reply = s.api("set-name", target="pane", id=p["id"], name="agent: gbos")
        s.settle(80)
        check("the verb reports ok", reply.get("ok") is True, str(reply))
        check(
            "and the name is what the frame says",
            titles(s) == ["agent: gbos"],
            str(titles(s)),
        )

        # The whole point: the program keeps shouting and is ignored.
        s.raw(r"\e]2;\xe2\xa0\xb9 Fix toast overlap in frame tests\x07")
        s.settle(120)
        check(
            "a program retitling itself cannot take the name back",
            titles(s) == ["agent: gbos"],
            str(titles(s)),
        )

        s.api("set-name", target="pane", id=p["id"], name="")
        s.settle(120)
        check(
            "clearing it hands the label back to the program",
            titles(s) == ["\u2839 Fix toast overlap in frame tests"],
            str(titles(s)),
        )


def test_the_verb_and_the_gesture_are_one_store():
    """A name set by a script is the name the editor offers to edit -- there is one
    place a pane's name lives, so the two ways in cannot disagree about it."""
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        s.api("set-name", target="pane", id=p["id"], name="named")
        s.settle(80)
        dbl(s, col_of(s.snapshot(), p, "named"), p["y"])
        s.settle(80)
        row = s.snapshot().line(p["y"])
        check(
            "the editor opens seeded with the scripted name",
            "named" in row and CURSOR in row,
            repr(row.strip()),
        )
        s.send(r"\e")
        s.settle(40)


def test_naming_addresses_the_focused_pane_by_default():
    """0 is the focused pane, as it is for every other verb that takes an id."""
    with Session(SH, cols=80, rows=30) as s:
        s.settle()
        s.key("-")  # two panes, focus lands on the new one
        s.settle(60)
        panes = s.panes()
        check("there are two panes to choose between", len(panes) == 2, str(panes))
        focused = [q for q in panes if q["focused"]][0]
        other = [q for q in panes if not q["focused"]][0]

        s.api("set-name", target="pane", name="the focused one")
        s.settle(80)
        by_id = {q["id"]: q["title"] for q in s.panes()}
        check(
            "id 0 means the focused pane",
            by_id[focused["id"]] == "the focused one",
            str(by_id),
        )
        check(
            "and the other pane keeps the title it had",
            by_id[other["id"]] == other["title"],
            str(by_id),
        )


def test_the_target_says_which_label_and_bad_ids_say_which_thing():
    """`set-name` has always meant the tab, so that stays its default -- a verb
    quietly changing its subject would be worse than two verbs whose defaults
    differ. The refusals name the thing that was not found, so a mistyped target is
    obvious from the reply rather than from a label that did not move."""
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        tab = s.tabs()[0]

        s.api("set-name", id=tab["id"], name="work")
        s.settle(60)
        check(
            "no target still names the tab",
            s.tabs()[0]["name"] == "work",
            str(s.tabs()[0]),
        )
        check(
            "...and leaves the pane's title alone",
            titles(s) == ["shell"],
            str(titles(s)),
        )

        check(
            "a pane id that does not exist says so",
            s.api("set-name", target="pane", id=4242, name="x").get("error")
            == "no such pane",
            str(s.api("set-name", target="pane", id=4242, name="x")),
        )
        check(
            "and a tab id that does not exist says the other thing",
            s.api("set-name", id=4242, name="x").get("error") == "no such tab",
            str(s.api("set-name", id=4242, name="x")),
        )
        check(
            "the pane kept its title through both refusals",
            titles(s) == ["shell"] and s.pane()["id"] == p["id"],
            str(titles(s)),
        )


def test_slow_clicks_are_not_a_double_click():
    """The control for the gesture: two clicks are not automatically a rename."""
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        x = col_of(s.snapshot(), p, "shell")
        s.click(x, p["y"])
        s.settle(80)
        time.sleep(0.6)  # default double_click_ms is 400
        s.click(x, p["y"])
        s.settle(80)
        row = s.snapshot().line(p["y"])
        check("two slow clicks open no editor", CURSOR not in row, repr(row))
        check("and still split nothing", len(s.panes()) == 1, str(len(s.panes())))


def test_typing_a_name_does_not_reach_the_pane():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        p = s.pane()
        open_editor(s, p)
        backspace(s, 5)
        s.send("zzz")
        s.send(r"\r")
        s.settle(120)
        body = s.snapshot().pane_text(s.pane())
        check(
            "the editor owns the keyboard: nothing leaked to the shell",
            "zzz" not in body,
            repr(body),
        )


def test_closing_the_pane_does_not_wedge_the_keyboard():
    with Session(SH, cols=70, rows=16) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left, right = s.panes()
        target = left if not left["focused"] else right
        open_editor(s, target)
        s.api("close", id=target["id"])
        s.settle(80)
        check("the pane is gone", len(s.panes()) == 1, str(len(s.panes())))
        # If the rename still owned the keyboard, this would be swallowed.
        s.send("after")
        s.settle(120)
        body = s.snapshot().pane_text(s.pane())
        check(
            "and the keyboard went back to the surviving pane",
            "after" in body,
            repr(body),
        )


if __name__ == "__main__":
    test_the_title_text_is_its_own_region()
    test_resting_on_the_title_arms_no_guide()
    test_clicking_the_title_does_not_split()
    test_double_clicking_the_title_does_not_split()
    test_the_title_still_drags_the_pane()
    test_hovering_the_title_still_focuses()
    test_double_click_opens_an_editor_seeded_with_the_name()
    test_enter_keeps_the_new_name()
    test_escape_abandons_it()
    test_clicking_away_keeps_the_name()
    test_a_typed_name_outranks_the_program()
    test_slow_clicks_are_not_a_double_click()
    test_typing_a_name_does_not_reach_the_pane()
    test_closing_the_pane_does_not_wedge_the_keyboard()
    test_a_script_can_name_a_pane_too()
    test_the_verb_and_the_gesture_are_one_store()
    test_naming_addresses_the_focused_pane_by_default()
    test_the_target_says_which_label_and_bad_ids_say_which_thing()
    sys.exit(report())
