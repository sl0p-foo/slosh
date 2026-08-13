#!/usr/bin/env python3
"""M3: tabs, purposes and the JSON control API."""
import sys

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
    with Session(SH, cols=60, rows=14) as s:
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


if __name__ == "__main__":
    test_tabs()
    test_background_tabs_keep_running()
    test_tab_click()
    test_closing_a_tab()
    test_purpose_trust_model()
    test_json_api()
    sys.exit(report())
