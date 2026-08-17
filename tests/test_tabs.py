#!/usr/bin/env python3
"""M3: tabs, purposes and the JSON control API."""

import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]

NEXT, PREV = r"\t", r"\e[Z"  # C-a tab / C-a shift+tab


def test_tabs():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        check("one tab to start", len(s.tabs()) == 1)

        s.raw("in-tab-one")
        s.settle()
        s.key("c")  # new tab
        s.settle()
        tabs = s.tabs()
        check("C-a c makes a tab", len(tabs) == 2, str(tabs))
        check("the new tab is active", tabs[1]["active"] and not tabs[0]["active"])
        check(
            "each tab has its own pane", [t["panes"] for t in tabs] == [1, 1], str(tabs)
        )

        snap = s.snapshot()
        check("the new tab's pane is empty", "in-tab-one" not in snap.screen())
        check(
            "the tab strip is drawn",
            "1" in snap.line(1) and "2" in snap.line(1),
            repr(snap.line(1)),
        )

        s.key(PREV)
        s.settle()
        check("C-a shift+tab goes back", s.tabs()[0]["active"])
        check(
            "the first tab kept its content",
            "in-tab-one" in s.snapshot().screen(),
            repr(s.snapshot().screen()[:200]),
        )

        s.key("2")
        s.settle()
        check("C-a 2 selects by number", s.tabs()[1]["active"])

        # `n`/`p` used to cycle. `p` is the palette now -- pressed far more
        # often than "the tab before this one" -- and `n` went with it rather
        # than leaving half a pair behind.
        active = [t["index"] for t in s.tabs() if t["active"]]
        s.key("n")
        s.settle()
        check(
            "C-a n no longer cycles tabs",
            [t["index"] for t in s.tabs() if t["active"]] == active,
            str(s.tabs()),
        )
        s.key("p")
        s.settle()
        check(
            "nor does C-a p, which opens the palette",
            [t["index"] for t in s.tabs() if t["active"]] == active,
            str(s.tabs()),
        )
        s.send(r"\e")  # put the palette away again


def test_background_tabs_keep_running():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        s.key("c")
        s.settle()
        first = [p for p in s.panes() if p["tab"] == 1][0]
        # drive the *background* tab's pane through the API
        s.api("focus", id=first["id"])
        s.settle()
        s.raw("still-alive")
        s.settle()
        s.key(NEXT)  # away again
        s.settle()
        check(
            "a background pane still accepts input",
            any(p["tab"] == 1 for p in s.panes()),
            str(s.panes()),
        )
        s.api("select-tab", index=1)
        s.settle()
        check(
            "its output survived being off-screen",
            "still-alive" in s.snapshot().screen(),
            repr(s.snapshot().screen()[:200]),
        )


def test_tab_click():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        s.key("c")
        s.settle()
        snap = s.snapshot()
        pos = snap.find("1")  # the first tab's label, on the strip row
        check("tab label is on the strip", pos is not None and pos[1] == 1, str(pos))
        if pos:
            x, y = pos
            action = snap.hit_at(x, y)
            check("the tab label is clickable", action == "tab:1", str(action))
            s.click(x, y)
            s.settle()
            check("clicking a tab switches to it", s.tabs()[0]["active"], str(s.tabs()))


def test_closing_a_tab():
    with Session(SH, cols=76, rows=14) as s:
        s.settle()
        s.key("c")
        s.settle()
        s.key("\\\\")  # two panes in tab 2
        s.settle()
        check("tab 2 has two panes", s.tabs()[1]["panes"] == 2, str(s.tabs()))
        s.key("x")
        s.settle()
        check("closing one pane keeps the tab", len(s.tabs()) == 2, str(s.tabs()))
        s.key("x")
        s.settle()
        check("closing the last pane closes the tab", len(s.tabs()) == 1, str(s.tabs()))
        check(
            "the surviving tab is active and usable",
            s.tabs()[0]["active"] and s.api("alive")["alive"],
        )


def test_closing_a_whole_tab_is_one_verb():
    """Closing a tab used to be a *side effect*: press `x` once per pane and the
    tab evaporates when the last one goes. Four panes meant four presses and
    counting, so the thing you meant had no way to be said."""
    # Room for three panes: a rows split inside a cols split needs the height,
    # and `min_split` refuses one that would not fit.
    with Session(SH, cols=90, rows=24) as s:
        s.settle()
        s.key("c")
        s.settle()
        s.key("\\\\")
        s.key("-")
        s.settle()
        check("tab 2 has three panes", s.tabs()[1]["panes"] == 3, str(s.tabs()))

        s.key("X")
        s.settle()
        check("one press closes the whole tab", len(s.tabs()) == 1, str(s.tabs()))
        screen = s.snapshot().screen()
        check(
            "and says how much went with it",
            "closed tab" in screen and "3 panes" in screen,
            screen,
        )
        check("the session is still running", s.api("alive")["alive"])


def test_the_last_tab_is_refused_rather_than_obeyed():
    """`close-tab` over the socket ends the session when nothing is left, which is
    right for a request. A key that closes a tab four times and ends your session
    the fifth is one you cannot press without counting first."""
    with Session(SH, cols=76, rows=14) as s:
        s.settle()
        s.key("X")
        s.settle()
        check("the tab is still there", len(s.tabs()) == 1, str(s.tabs()))
        check("the session is still running", s.api("alive")["alive"])
        check(
            "and it says what to press instead",
            "quit" in s.snapshot().screen(),
            s.snapshot().screen(),
        )


def test_close_tab_is_reachable_without_the_key():
    with Session(SH, cols=92, rows=36) as s:
        s.settle()
        s.send(r"\x01?")
        s.settle()
        check(
            "the cheatsheet lists it",
            "close this tab" in s.snapshot().screen(),
            s.snapshot().screen(),
        )
        s.send(r"\x01q")  # any key dismisses the sheet; this one is swallowed
        s.settle()
        s.key("p")
        s.send("close this tab")
        check(
            "so does the palette",
            "close this tab" in s.snapshot().screen(),
            s.snapshot().screen(),
        )


def test_purpose_trust_model():
    """D8: a declared purpose outranks an in-band one and cannot be overridden."""
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        pane = s.pane()

        r = s.api(
            "set-purpose",
            target="pane",
            id=pane["id"],
            purpose="agent:main",
            declared=True,
        )
        check("a declared purpose is accepted", r["ok"], str(r))
        p = s.pane()
        check("the purpose is reported", p["purpose"] == "agent:main", str(p))
        check("and marked declared", p["purpose_declared"], str(p))

        r = s.api(
            "set-purpose",
            target="pane",
            id=pane["id"],
            purpose="evil:overridden",
            declared=False,
        )
        check("an in-band purpose cannot override a declared one", not r["ok"], str(r))
        check(
            "the declared purpose is intact",
            s.pane()["purpose"] == "agent:main",
            str(s.pane()),
        )

        r = s.api(
            "set-purpose",
            target="tab",
            id=s.tabs()[0]["id"],
            purpose="project:foo bar/../$(rm -rf)",
            declared=True,
        )
        check(
            "a tab purpose is sanitised on ingest",
            s.tabs()[0]["purpose"] == "project:foobar/../rm-rf",
            str(s.tabs()[0]["purpose"]),
        )

    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        pane = s.pane()
        r = s.api(
            "set-purpose",
            target="pane",
            id=pane["id"],
            purpose="guessed:by-the-pane",
            declared=False,
        )
        check("an in-band purpose is allowed when nothing is declared", r["ok"], str(r))
        check(
            "and does not lock the slot",
            not s.pane()["purpose_declared"],
            str(s.pane()),
        )


def test_json_api():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        base = s.pane()["id"]

        r = s.api("split", dir="rows", id=base)
        check("split answers with the new pane id", r["ok"] and r["id"] != base, str(r))
        new = r["id"]

        r = s.api("focus", id=base)
        check("focus by id works", r["ok"] and r["id"] == base, str(r))
        check(
            "focus by id is reflected in the layout",
            [p for p in s.panes() if p["focused"]][0]["id"] == base,
        )

        r = s.api("new-tab", name="build", purpose="project:x.deadbeef")
        check("new-tab answers with an id", r["ok"] and r["id"], str(r))
        t = [t for t in s.tabs() if t["id"] == r["id"]][0]
        check(
            "new-tab records name and declared purpose",
            t["name"] == "build"
            and t["purpose"] == "project:x.deadbeef"
            and t["purpose_declared"],
            str(t),
        )

        r = s.api("snapshot", format="text")
        check("snapshot text comes back as JSON", r["ok"] and "text" in r, str(r)[:80])

        r = s.api("close", id=new)
        check("close by id works", r["ok"], str(r))

        check(
            "unknown commands are refused, not fatal",
            s.api("nope")["error"] == "unknown cmd",
        )
        r = s.api("focus", id=9999)
        check(
            "bad ids are refused, not fatal",
            not r["ok"] and s.api("alive")["ok"],
            str(r),
        )


# ---- hovering the strip ----------------------------------------------------

ACCENT = "#ff5fd7"  # the fill of the tab you are in, and hover on the rest
INK = "#141418"  # text on that fill
BRIGHT = "#ffffff"  # that text while the pointer is on it
DIM = "#45454a"  # a tab you are not in


def two_tab_layout():
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(
        'layout {\n tab name="api" {\n  pane\n }\n tab name="notes" {\n  pane\n }\n}\n'
    )
    f.close()
    return f.name


def strip_hits(snap):
    return {
        h["action"]: h
        for h in snap.hits
        if h["action"].startswith("tab:") or h["action"] == "newtab"
    }


def hover(s, x, y):
    s.send(rf"\e[<35;{x + 1};{y + 1}M")


def fg_of(snap, h):
    return (snap.style_at(h["x"] + 1, h["y"]) or {}).get("fg")


def bg_of(snap, h):
    return (snap.style_at(h["x"] + 1, h["y"]) or {}).get("bg")


def attrs_of(snap, h):
    return (snap.style_at(h["x"] + 1, h["y"]) or {}).get("attrs", [])


def test_hovering_the_strip_lights_what_is_under_the_pointer():
    lay = two_tab_layout()
    with Session(SH, cols=80, rows=14, layout=lay) as s:
        s.settle(20)
        hits = strip_hits(s.snapshot())
        check("the strip has two tabs and a new-tab button", len(hits) == 3, str(hits))

        active_id = "tab:" + str(s.tabs()[0]["id"])
        for target, h in hits.items():
            hover(s, h["x"] + 1, h["y"])
            snap = s.snapshot()
            # The tab you are in keeps its fill and brightens its text; the
            # others have no fill to keep and take the accent.
            want = BRIGHT if target == active_id else ACCENT
            check(
                f"hovering {target} lights it",
                fg_of(snap, h) == want,
                f"{fg_of(snap, h)} wanted {want}",
            )
            others = [(k, fg_of(snap, o)) for k, o in hits.items() if k != target]
            check(
                "and nothing else on the strip",
                all(c in (INK, DIM) for _, c in others),
                str(others),
            )
    os.unlink(lay)


def test_weight_says_active_and_colour_says_pointer():
    """Two independent signals, so hovering the tab you are in does not make it
    look like you left it."""
    lay = two_tab_layout()
    with Session(SH, cols=80, rows=14, layout=lay) as s:
        s.settle(20)
        hits = strip_hits(s.snapshot())
        active = hits["tab:" + str(s.tabs()[0]["id"])]

        snap = s.snapshot()
        check(
            "the tab you are in is filled, not merely bold",
            fg_of(snap, active) == INK and bg_of(snap, active) == ACCENT,
            f"{fg_of(snap, active)} on {bg_of(snap, active)}",
        )
        check(
            "and still bold",
            "bold" in attrs_of(snap, active),
            str(attrs_of(snap, active)),
        )

        inactive = [
            h for k, h in hits.items() if k.startswith("tab:") and h is not active
        ][0]
        check(
            "a tab you are not in has no fill at all",
            bg_of(snap, inactive) is None and fg_of(snap, inactive) == DIM,
            f"{fg_of(snap, inactive)} on {bg_of(snap, inactive)}",
        )

        hover(s, active["x"] + 1, active["y"])
        snap = s.snapshot()
        check(
            "hovering it brightens the text",
            fg_of(snap, active) == BRIGHT,
            str(fg_of(snap, active)),
        )
        check(
            "but it keeps the fill that says you are in it",
            bg_of(snap, active) == ACCENT,
            str(bg_of(snap, active)),
        )
    os.unlink(lay)


def test_leaving_the_strip_puts_it_out_and_hover_switches_nothing():
    lay = two_tab_layout()
    with Session(SH, cols=80, rows=14, layout=lay) as s:
        s.settle(20)
        before = s.tabs()
        hits = strip_hits(s.snapshot())
        other = [h for k, h in hits.items() if k.startswith("tab:")][1]

        hover(s, other["x"] + 1, other["y"])
        s.settle(20)
        check(
            "hovering a tab does not switch to it",
            [t["active"] for t in s.tabs()] == [t["active"] for t in before],
            str(s.tabs()),
        )

        p = s.pane()
        hover(s, p["content_x"] + 2, p["content_y"] + 1)
        snap = s.snapshot()
        check(
            "nothing on the strip is lit once the pointer leaves it",
            all(fg_of(snap, h) in (INK, DIM) for h in hits.values()),
            str([fg_of(snap, h) for h in hits.values()]),
        )

        # ...but clicking still switches.
        s.click(other["x"] + 1, other["y"])
        s.settle(20)
        check(
            "clicking the tab you lit is what switches to it",
            [t["active"] for t in s.tabs()] != [t["active"] for t in before],
            str(s.tabs()),
        )
    os.unlink(lay)


def dbl(s, h):
    """Two clicks on the same cell, close enough together to be a double."""
    s.click(h["x"] + 1, h["y"])
    s.click(h["x"] + 1, h["y"])


def press(s, x, y):
    s.send(rf"\e[<0;{x + 1};{y + 1}M")


def motion(s, x, y):
    s.send(rf"\e[<32;{x + 1};{y + 1}M")


def release(s, x, y):
    s.send(rf"\e[<0;{x + 1};{y + 1}m")


def named_tabs(s, *names):
    """A session with one unnamed tab and one per name, in order."""
    for n in names:
        s.key("c")
        s.settle()
        s.api("set-name", id=s.tabs()[-1]["id"], name=n)
    return [t["id"] for t in s.tabs()]


def order(s):
    return [t["name"] for t in s.tabs()]


def active(s):
    return [t["name"] for t in s.tabs() if t["active"]][0]


def test_dragging_a_tab_reorders_the_strip():
    with Session(SH, cols=80, rows=12) as s:
        s.settle()
        ids = named_tabs(s, "one", "two", "three")
        check(
            "the order to start", order(s) == ["", "one", "two", "three"], str(order(s))
        )

        hits = strip_hits(s.snapshot())
        src = hits[f"tab:{ids[3]}"]
        dst = hits[f"tab:{ids[1]}"]
        press(s, src["x"] + 1, src["y"])
        motion(s, dst["x"] + 1, dst["y"])
        release(s, dst["x"] + 1, dst["y"])

        check(
            "the dragged tab lands where it was dropped",
            order(s) == ["", "three", "one", "two"],
            str(order(s)),
        )
        check("and you are still in it", active(s) == "three", active(s))


def test_dragging_past_the_last_tab_puts_it_last():
    with Session(SH, cols=80, rows=12) as s:
        s.settle()
        ids = named_tabs(s, "one", "two")
        hits = strip_hits(s.snapshot())
        src = hits[f"tab:{ids[0]}"]
        end = hits["newtab"]
        press(s, src["x"] + 1, src["y"])
        motion(s, end["x"] + 1, end["y"])
        release(s, end["x"] + 1, end["y"])
        check(
            "dropping past the end means last",
            order(s) == ["one", "two", ""],
            str(order(s)),
        )


def test_dragging_off_the_strip_moves_nothing():
    """Not a cancel and not a move: leaving the strip is simply not a place to
    put a tab, so the last position it reached is where it stays."""
    with Session(SH, cols=80, rows=12) as s:
        s.settle()
        ids = named_tabs(s, "one", "two")
        hits = strip_hits(s.snapshot())
        src = hits[f"tab:{ids[2]}"]
        press(s, src["x"] + 1, src["y"])
        motion(s, 20, 6)  # into a pane, well below the strip
        release(s, 20, 6)
        check("the order is untouched", order(s) == ["", "one", "two"], str(order(s)))


def test_a_click_that_never_moves_only_switches():
    with Session(SH, cols=80, rows=12) as s:
        s.settle()
        ids = named_tabs(s, "one", "two")
        hits = strip_hits(s.snapshot())
        h = hits[f"tab:{ids[1]}"]
        press(s, h["x"] + 1, h["y"])
        release(s, h["x"] + 1, h["y"])
        check("still in order", order(s) == ["", "one", "two"], str(order(s)))
        check("and it switched", active(s) == "one", active(s))


def test_move_tab_over_the_api_keeps_you_where_you_are():
    """`cur` is a position, not an identity, so a move has to recompute it.
    Asserted from a tab that is *not* the one being moved, which is the case
    the drag can never produce and the arithmetic can still get wrong."""
    with Session(SH, cols=80, rows=12) as s:
        s.settle()
        ids = named_tabs(s, "a", "b", "c")
        s.api("select-tab", index=2)  # sit in "a"
        check("sitting in a", active(s) == "a", active(s))

        r = s.api("move-tab", id=ids[3], index=1)  # move "c" to the front
        check("the api reports ok", r.get("ok") is True, str(r))
        check("the strip is reordered", order(s) == ["c", "", "a", "b"], str(order(s)))
        check("and you are still in the tab you were in", active(s) == "a", active(s))

        check(
            "an unknown tab is refused",
            s.api("move-tab", id=999, index=1).get("ok") is False,
        )
        check(
            "and so is an index off the end",
            s.api("move-tab", id=ids[0], index=99).get("ok") is False,
        )


def test_double_clicking_a_tab_renames_it():
    """The same gesture as a pane's title, on the other thing that has a name.

    Tabs could be named by a layout or the control API and by nothing else, so
    a tab you made yourself was stuck being a number."""
    with Session(SH, cols=70, rows=12) as s:
        s.settle()
        s.key("c")
        s.settle()
        tab2 = "tab:" + str(s.tabs()[1]["id"])

        dbl(s, strip_hits(s.snapshot())[tab2])
        snap = s.snapshot()
        check(
            "the editor opens in the tab's own cell",
            "\u2588" in snap.line(1),
            repr(snap.line(1)),
        )

        s.send("logs")
        check(
            "typing goes into it",
            "logs\u2588" in s.snapshot().line(1),
            repr(s.snapshot().line(1)),
        )

        s.send(r"\r")
        check("enter keeps it", s.tabs()[1]["name"] == "logs", str(s.tabs()))
        check(
            "and the strip shows it",
            "2:logs" in s.snapshot().line(1),
            repr(s.snapshot().line(1)),
        )


def test_escape_abandons_and_clicking_away_keeps():
    with Session(SH, cols=70, rows=12) as s:
        s.settle()
        s.key("c")
        s.settle()
        tab2 = "tab:" + str(s.tabs()[1]["id"])

        dbl(s, strip_hits(s.snapshot())[tab2])
        s.send("throwaway")
        s.send(r"\x1b")
        check("escape leaves the name alone", s.tabs()[1]["name"] == "", str(s.tabs()))
        check("and closes the editor", "\u2588" not in s.snapshot().screen())

        dbl(s, strip_hits(s.snapshot())[tab2])
        s.send("kept")
        s.click(2, 6)  # somewhere that is not this tab
        check(
            "clicking away commits, the way leaving a field does",
            s.tabs()[1]["name"] == "kept",
            str(s.tabs()),
        )

        # And an empty name gives the tab back to its number.
        dbl(s, strip_hits(s.snapshot())[tab2])
        for _ in range(len("kept")):
            s.send(r"\x7f")
        s.send(r"\r")
        check("an empty name clears it", s.tabs()[1]["name"] == "", str(s.tabs()))


def test_a_tab_and_a_pane_with_the_same_id_are_not_the_same_thing():
    """Pane ids and tab ids are separate sequences, so both `2`s exist. A click
    on one followed by a click on the other must not read as a double-click on
    either."""
    # A pane only registers a `panetitle:` hit when it has a title to draw.
    titled = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']
    with Session(titled, cols=70, rows=12) as s:
        s.settle()
        s.key("c")
        s.until(lambda sn: any(h["action"].startswith("panetitle:") for h in sn.hits))
        snap = s.snapshot()
        pane_id = s.focused()["id"]  # the new tab's pane, not tab 1's
        tab_id = s.tabs()[1]["id"]
        check(
            "the ids really do collide",
            pane_id == tab_id,
            f"pane {pane_id} tab {tab_id}",
        )

        title = [h for h in snap.hits if h["action"] == f"panetitle:{pane_id}"][0]
        strip = strip_hits(snap)[f"tab:{tab_id}"]

        s.click(strip["x"] + 1, strip["y"])
        s.click(title["x"] + 1, title["y"])
        check(
            "one click on each opens no editor",
            "\u2588" not in s.snapshot().screen(),
            s.snapshot().screen(),
        )

        dbl(s, title)
        check(
            "but two on the pane still open its own",
            "\u2588" in s.snapshot().screen(),
            s.snapshot().screen(),
        )


if __name__ == "__main__":
    test_tabs()
    test_background_tabs_keep_running()
    test_tab_click()
    test_closing_a_tab()
    test_closing_a_whole_tab_is_one_verb()
    test_the_last_tab_is_refused_rather_than_obeyed()
    test_close_tab_is_reachable_without_the_key()
    test_purpose_trust_model()
    test_json_api()
    test_hovering_the_strip_lights_what_is_under_the_pointer()
    test_weight_says_active_and_colour_says_pointer()
    test_leaving_the_strip_puts_it_out_and_hover_switches_nothing()
    test_double_clicking_a_tab_renames_it()
    test_escape_abandons_and_clicking_away_keeps()
    test_a_tab_and_a_pane_with_the_same_id_are_not_the_same_thing()
    test_dragging_a_tab_reorders_the_strip()
    test_dragging_past_the_last_tab_puts_it_last()
    test_dragging_off_the_strip_moves_nothing()
    test_a_click_that_never_moves_only_switches()
    test_move_tab_over_the_api_keeps_you_where_you_are()
    sys.exit(report())
