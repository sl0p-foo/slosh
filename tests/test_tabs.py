#!/usr/bin/env python3
"""M3: tabs, purposes and the JSON control API."""
import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]


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
        check("each tab has its own pane", [t["panes"] for t in tabs] == [1, 1],
              str(tabs))

        snap = s.snapshot()
        check("the new tab's pane is empty", "in-tab-one" not in snap.screen())
        check("the tab strip is drawn", "1" in snap.line(1) and "2" in snap.line(1),
              repr(snap.line(1)))

        s.key("p")  # previous tab
        s.settle()
        check("C-a p goes back", s.tabs()[0]["active"])
        check("the first tab kept its content", "in-tab-one" in s.snapshot().screen(),
              repr(s.snapshot().screen()[:200]))

        s.key("2")
        s.settle()
        check("C-a 2 selects by number", s.tabs()[1]["active"])


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
        s.key("n")  # away again
        s.settle()
        check("a background pane still accepts input",
              any(p["tab"] == 1 for p in s.panes()), str(s.panes()))
        s.api("select-tab", index=1)
        s.settle()
        check("its output survived being off-screen",
              "still-alive" in s.snapshot().screen(),
              repr(s.snapshot().screen()[:200]))


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
            check("clicking a tab switches to it", s.tabs()[0]["active"],
                  str(s.tabs()))


def test_closing_a_tab():
    with Session(SH, cols=76, rows=14) as s:
        s.settle()
        s.key("c")
        s.settle()
        s.key("\\\\")   # two panes in tab 2
        s.settle()
        check("tab 2 has two panes", s.tabs()[1]["panes"] == 2, str(s.tabs()))
        s.key("x")
        s.settle()
        check("closing one pane keeps the tab", len(s.tabs()) == 2, str(s.tabs()))
        s.key("x")
        s.settle()
        check("closing the last pane closes the tab", len(s.tabs()) == 1,
              str(s.tabs()))
        check("the surviving tab is active and usable",
              s.tabs()[0]["active"] and s.api("alive")["alive"])


def test_purpose_trust_model():
    """D8: a declared purpose outranks an in-band one and cannot be overridden."""
    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        pane = s.pane()

        r = s.api("set-purpose", target="pane", id=pane["id"],
                  purpose="agent:main", declared=True)
        check("a declared purpose is accepted", r["ok"], str(r))
        p = s.pane()
        check("the purpose is reported", p["purpose"] == "agent:main", str(p))
        check("and marked declared", p["purpose_declared"], str(p))

        r = s.api("set-purpose", target="pane", id=pane["id"],
                  purpose="evil:overridden", declared=False)
        check("an in-band purpose cannot override a declared one", not r["ok"],
              str(r))
        check("the declared purpose is intact",
              s.pane()["purpose"] == "agent:main", str(s.pane()))

        r = s.api("set-purpose", target="tab", id=s.tabs()[0]["id"],
                  purpose="project:foo bar/../$(rm -rf)", declared=True)
        check("a tab purpose is sanitised on ingest",
              s.tabs()[0]["purpose"] == "project:foobar/../rm-rf",
              str(s.tabs()[0]["purpose"]))

    with Session(SH, cols=60, rows=12) as s:
        s.settle()
        pane = s.pane()
        r = s.api("set-purpose", target="pane", id=pane["id"],
                  purpose="guessed:by-the-pane", declared=False)
        check("an in-band purpose is allowed when nothing is declared", r["ok"],
              str(r))
        check("and does not lock the slot",
              not s.pane()["purpose_declared"], str(s.pane()))


def test_json_api():
    with Session(SH, cols=60, rows=14) as s:
        s.settle()
        base = s.pane()["id"]

        r = s.api("split", dir="rows", id=base)
        check("split answers with the new pane id", r["ok"] and r["id"] != base,
              str(r))
        new = r["id"]

        r = s.api("focus", id=base)
        check("focus by id works", r["ok"] and r["id"] == base, str(r))
        check("focus by id is reflected in the layout",
              [p for p in s.panes() if p["focused"]][0]["id"] == base)

        r = s.api("new-tab", name="build", purpose="project:x.deadbeef")
        check("new-tab answers with an id", r["ok"] and r["id"], str(r))
        t = [t for t in s.tabs() if t["id"] == r["id"]][0]
        check("new-tab records name and declared purpose",
              t["name"] == "build" and t["purpose"] == "project:x.deadbeef"
              and t["purpose_declared"], str(t))

        r = s.api("snapshot", format="text")
        check("snapshot text comes back as JSON", r["ok"] and "text" in r,
              str(r)[:80])

        r = s.api("close", id=new)
        check("close by id works", r["ok"], str(r))

        check("unknown commands are refused, not fatal",
              s.api("nope")["error"] == "unknown cmd")
        r = s.api("focus", id=9999)
        check("bad ids are refused, not fatal", not r["ok"] and s.api("alive")["ok"],
              str(r))


# ---- hovering the strip ----------------------------------------------------

ACCENT = "#ff5fd7"      # the fill of the tab you are in, and hover on the rest
INK = "#141418"         # text on that fill
BRIGHT = "#ffffff"      # that text while the pointer is on it
DIM = "#45454a"         # a tab you are not in


def two_tab_layout():
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write('layout {\n tab name="api" {\n  pane\n }\n'
            ' tab name="notes" {\n  pane\n }\n}\n')
    f.close()
    return f.name


def strip_hits(snap):
    return {h["action"]: h for h in snap.hits
            if h["action"].startswith("tab:") or h["action"] == "newtab"}


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
        check("the strip has two tabs and a +tab", len(hits) == 3, str(hits))

        active_id = "tab:" + str(s.tabs()[0]["id"])
        for target, h in hits.items():
            hover(s, h["x"] + 1, h["y"])
            snap = s.snapshot()
            # The tab you are in keeps its fill and brightens its text; the
            # others have no fill to keep and take the accent.
            want = BRIGHT if target == active_id else ACCENT
            check(f"hovering {target} lights it",
                  fg_of(snap, h) == want, f"{fg_of(snap, h)} wanted {want}")
            others = [(k, fg_of(snap, o)) for k, o in hits.items() if k != target]
            check("and nothing else on the strip",
                  all(c in (INK, DIM) for _, c in others), str(others))
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
        check("the tab you are in is filled, not merely bold",
              fg_of(snap, active) == INK and bg_of(snap, active) == ACCENT,
              f"{fg_of(snap, active)} on {bg_of(snap, active)}")
        check("and still bold", "bold" in attrs_of(snap, active),
              str(attrs_of(snap, active)))

        inactive = [h for k, h in hits.items()
                    if k.startswith("tab:") and h is not active][0]
        check("a tab you are not in has no fill at all",
              bg_of(snap, inactive) is None and fg_of(snap, inactive) == DIM,
              f"{fg_of(snap, inactive)} on {bg_of(snap, inactive)}")

        hover(s, active["x"] + 1, active["y"])
        snap = s.snapshot()
        check("hovering it brightens the text",
              fg_of(snap, active) == BRIGHT, str(fg_of(snap, active)))
        check("but it keeps the fill that says you are in it",
              bg_of(snap, active) == ACCENT, str(bg_of(snap, active)))
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
        check("hovering a tab does not switch to it",
              [t["active"] for t in s.tabs()] ==
              [t["active"] for t in before], str(s.tabs()))

        p = s.pane()
        hover(s, p["content_x"] + 2, p["content_y"] + 1)
        snap = s.snapshot()
        check("nothing on the strip is lit once the pointer leaves it",
              all(fg_of(snap, h) in (INK, DIM) for h in hits.values()),
              str([fg_of(snap, h) for h in hits.values()]))

        # ...but clicking still switches.
        s.click(other["x"] + 1, other["y"])
        s.settle(20)
        check("clicking the tab you lit is what switches to it",
              [t["active"] for t in s.tabs()] !=
              [t["active"] for t in before], str(s.tabs()))
    os.unlink(lay)


if __name__ == "__main__":
    test_tabs()
    test_background_tabs_keep_running()
    test_tab_click()
    test_closing_a_tab()
    test_purpose_trust_model()
    test_json_api()
    test_hovering_the_strip_lights_what_is_under_the_pointer()
    test_weight_says_active_and_colour_says_pointer()
    test_leaving_the_strip_puts_it_out_and_hover_switches_nothing()
    sys.exit(report())
