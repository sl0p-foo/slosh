#!/usr/bin/env python3
"""M7: layout files — the prerequisite for porting `sl0ppi up`.

A layout declares tabs, purposes and panes; the purposes it declares are
locked, and commands can be suspended so that opening twelve projects does not
start twelve servers.
"""
import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]

LAYOUT = """
layout {
    tab name="api" purpose="project:api.a1b2" cwd="/tmp" {
        pane purpose="agent:main" command="stty raw -echo; cat"
        pane split="rows" {
            pane purpose="service:web" command="stty raw -echo; printf web-running; cat" suspended=true
            pane purpose="logs:tail" command="stty raw -echo; cat"
        }
    }
    tab name="notes" {
        pane
    }
}
"""


def layout_file(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def session_with(text, **kw):
    path = layout_file(text)
    s = Session(SH, layout=path, **kw)
    s._layout_path = path
    return s


def test_structure():
    with session_with(LAYOUT, cols=78, rows=18) as s:
        s.settle()
        tabs = s.tabs()
        check("a layout builds its tabs", [t["name"] for t in tabs] == ["api", "notes"],
              str(tabs))
        check("and only its tabs (the default one is replaced)", len(tabs) == 2,
              str(tabs))
        check("tab purposes are declared, so nothing can override them",
              tabs[0]["purpose"] == "project:api.a1b2" and tabs[0]["purpose_declared"],
              str(tabs[0]))

        panes = s.panes()
        check("panes are built in every tab", len(panes) == 4, str(len(panes)))
        purposes = [p["purpose"] for p in panes if p["tab"] == 1]
        check("pane purposes come from the layout",
              purposes == ["agent:main", "service:web", "logs:tail"], str(purposes))
        check("pane purposes are declared too",
              all(p["purpose_declared"] for p in panes if p["purpose"]), str(panes))

        first, web, logs = panes[0], panes[1], panes[2]
        check("the tab body splits into columns by default",
              first["x"] < web["x"] and first["y"] == web["y"],
              f"{first['x']},{first['y']} vs {web['x']},{web['y']}")
        check("a nested pane with split=rows stacks its children",
              web["x"] == logs["x"] and web["y"] < logs["y"],
              f"{web['y']} vs {logs['y']}")
    os.unlink(s._layout_path)


def test_suspended():
    with session_with(LAYOUT, cols=78, rows=18) as s:
        s.settle()
        panes = s.panes()
        web = [p for p in panes if p["purpose"] == "service:web"][0]
        check("a suspended pane is marked as such", web["suspended"], str(web))
        check("others are not",
              not [p for p in panes if p["purpose"] == "agent:main"][0]["suspended"])

        snap = s.snapshot()
        check("a suspended pane says what it would run",
              "press a key to run:" in snap.screen(), repr(snap.screen()[:400]))
        check("but has not run it", "web-running" not in snap.pane_text(web),
              repr(snap.pane_text(web)))

        s.api("focus", id=web["id"])
        s.send("x")
        s.settle()
        now = [p for p in s.panes() if p["id"] == web["id"]]
        check("a keystroke starts it", now and not now[0]["suspended"],
              str(s.panes()))
        check("and the command actually runs",
              "web-running" in s.snapshot().pane_text(now[0]),
              repr(s.snapshot().pane_text(now[0])))
        check("the keystroke that started it is not also sent to it",
              "x" not in s.snapshot().pane_text(now[0]).replace("web-running", ""),
              repr(s.snapshot().pane_text(now[0])))
        check("the session is unharmed", s.api("alive")["ok"])
    os.unlink(s._layout_path)


def test_declared_purposes_are_locked():
    with session_with(LAYOUT, cols=78, rows=18) as s:
        s.settle()
        agent = [p for p in s.panes() if p["purpose"] == "agent:main"][0]
        r = s.api("set-purpose", target="pane", id=agent["id"],
                  purpose="evil:relabelled", declared=False)
        check("a layout-declared pane purpose cannot be taken in-band",
              not r["ok"], str(r))
        check("it is unchanged",
              [p for p in s.panes() if p["id"] == agent["id"]][0]["purpose"]
              == "agent:main")

        r = s.api("set-purpose", target="tab", id=s.tabs()[0]["id"],
                  purpose="evil:tab", declared=False)
        check("nor a tab purpose", not r["ok"], str(r))
    os.unlink(s._layout_path)


def test_cwd():
    text = """
    layout {
        tab name="t" cwd="/tmp" {
            pane command="pwd; cat"
            pane cwd="/usr" command="pwd; cat"
        }
    }
    """
    with session_with(text, cols=70, rows=14) as s:
        s.settle()
        panes = s.panes()
        snap = s.snapshot()
        check("a tab's cwd is inherited by its panes",
              "/tmp" in snap.pane_text(panes[0]), repr(snap.pane_text(panes[0])))
        check("a pane can override it",
              "/usr" in snap.pane_text(panes[1]), repr(snap.pane_text(panes[1])))
    os.unlink(s._layout_path)


def test_apply_over_the_api():
    """How `sl0ppi up` will add a project tab to a running session."""
    with Session(SH, cols=70, rows=14) as s:
        s.settle()
        check("one tab to start", len(s.tabs()) == 1)

        frag = ('layout { tab name="added" purpose="project:x.deadbeef" '
                '{ pane purpose="agent:main" } }')
        r = s.api("apply-layout", kdl=frag)
        s.settle()
        check("apply-layout answers with the tab list", r["ok"] and "tabs" in r,
              str(r)[:100])
        check("the tab is added, not replacing what was there",
              len(s.tabs()) == 2 and s.tabs()[1]["name"] == "added", str(s.tabs()))
        check("its purpose is declared",
              s.tabs()[1]["purpose"] == "project:x.deadbeef"
              and s.tabs()[1]["purpose_declared"], str(s.tabs()[1]))

        # idempotency belongs to the caller: it diffs, then adds what is missing
        have = {t["purpose"] for t in s.tabs()}
        if "project:x.deadbeef" not in have:
            s.api("apply-layout", kdl=frag)
        check("a caller that diffs first does not duplicate", len(s.tabs()) == 2,
              str(s.tabs()))

        r = s.api("close-tab", id=s.tabs()[1]["id"])
        check("close-tab removes it", r["ok"] and len(s.tabs()) == 1, str(s.tabs()))

        r = s.api("apply-layout", kdl='layout { tab name="only" { pane } }',
                  replace=True)
        s.settle()
        check("replace=true rebuilds the session",
              len(s.tabs()) == 1 and s.tabs()[0]["name"] == "only", str(s.tabs()))


def test_bad_layouts():
    with Session(SH, cols=70, rows=14) as s:
        s.settle()
        r = s.api("apply-layout", kdl="layout { tab name=\"x\" ")
        check("a broken layout is refused", not r["ok"], str(r))
        check("with a line number", "line" in str(r.get("error", "")), str(r))

        r = s.api("apply-layout", kdl="layout { }")
        check("a layout with no tabs is refused", not r["ok"], str(r))

        r = s.api("apply-layout", path="/nonexistent/layout.kdl")
        check("a missing file is refused", not r["ok"], str(r))

        check("the session survived all of it",
              s.api("alive")["alive"] and len(s.tabs()) == 1, str(s.tabs()))

    broken = layout_file("layout { tab name=\"x\"")
    with Session(SH, cols=70, rows=14, layout=broken) as s:
        s.settle()
        check("a broken layout at startup still gives you a session",
              s.api("alive")["alive"] and len(s.panes()) == 1, str(s.panes()))
    os.unlink(broken)


if __name__ == "__main__":
    test_structure()
    test_suspended()
    test_declared_purposes_are_locked()
    test_cwd()
    test_apply_over_the_api()
    test_bad_layouts()
    sys.exit(report())
