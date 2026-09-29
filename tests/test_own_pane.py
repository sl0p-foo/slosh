#!/usr/bin/env python3
"""A program in a pane can act on *its own* pane, without using focus.

The bug this pins: everything a script could ask about a pane was answered
relative to focus, and focus is the human's. A tool spawning work "beside
itself" had nothing else to go on, so it read `panes` for whichever pane was
focused, and landed its work in whatever tab somebody else happened to be
looking at -- in a session with several agents in it, reliably the wrong one.
Worse, applying a layout jumped the view to the tab it built, so the *next*
caller read a focus that the previous spawn had moved.

So four things are checked here, which together are "background work lands where
the caller lives and nobody's view moves":

  * $SLOSH_PANE tells a program which pane it is in (and survives a rerun).
  * `panes` reports each pane's `pid`, so a process with no environment to read
    -- no controlling tty, a scrubbed env -- can still find itself by walking
    its own parents.
  * `apply-layout focus:false` builds tabs without going to them, and the panes
    it builds are still given a real size (a pane nobody looks at used to stay
    1x1, which was invisible only because applying a layout always jumped).
  * `move-pane beside:<id> focus:false` lands a pane next to a named pane and
    leaves focus, and the view, alone.
  * `raw id:<id>` and `capture id:<id>` drive and read a pane in another tab
    with no focus change and no select-tab.
"""

import os
import sys

from harness import Session, check, report

# A pane that reports its own identity into a file, then waits. This is the
# question under test: what does a program in a pane know about its own pane?
WHOAMI = "/tmp/slosh-test-whoami.txt"
SHELL = ["/bin/sh", "-c", f'echo "pane=$SLOSH_PANE pid=$$" > {WHOAMI}; read x']
# Something with visible output, for the capture checks.
TALKER = ["/bin/sh", "-c", "echo MARKER-ONE; read x"]


def layout(kdl):
    import tempfile

    f = tempfile.NamedTemporaryFile("w", suffix=".layout", delete=False)
    f.write(kdl)
    f.close()
    return f.name


def test_a_pane_is_told_which_pane_it_is():
    if os.path.exists(WHOAMI):
        os.unlink(WHOAMI)
    with Session(SHELL, cols=60, rows=12) as s:
        s.settle(200)
        pane = s.pane(0)
        said = open(WHOAMI).read().strip() if os.path.exists(WHOAMI) else "<nothing>"
        check(
            "$SLOSH_PANE is the pane's own id",
            f"pane={pane['id']} " in said + " ",
            f"pane id {pane['id']}, program said {said!r}",
        )
        check(
            "panes reports the pid of the program in the pane",
            f"pid={pane['pid']}" in said,
            f"panes says pid {pane['pid']}, program said {said!r}",
        )


def test_the_id_survives_a_rerun():
    """A pane's identity is the pane's, not the process's: `rerun` keeps it.

    Otherwise a tool that recorded "my pane is 3" would be talking about
    somebody else after the pane it meant was run again."""
    log = "/tmp/slosh-test-rerun.txt"
    if os.path.exists(log):
        os.unlink(log)
    # A *commanded* pane, because only those keep a husk to rerun -- a pane
    # that is just a shell goes when its shell does (see test_dead.py).
    lay = layout(
        'layout {\n  tab {\n    pane command="echo pane=$SLOSH_PANE >> %s"\n    pane\n  }\n}\n'
        % log
    )
    with Session(["/bin/sh", "-c", "read x"], cols=60, rows=12, layout=lay) as s:
        s.until(lambda sn: not s.pane(0)["alive"])
        pane_id = s.pane(0)["id"]
        s.api("rerun", id=pane_id)
        s.until(lambda sn: len(open(log).read().splitlines()) >= 2)
        said = open(log).read().split()
        check(
            "the same pane reports the same id after a rerun",
            said == [f"pane={pane_id}", f"pane={pane_id}"],
            f"pane {pane_id} logged {said}",
        )


def test_a_layout_can_be_applied_without_stealing_the_view():
    with Session(TALKER, cols=60, rows=12) as s:
        s.settle(100)
        s.api("new-tab", name="human-tab")
        s.api("select-tab", index=2)
        s.settle(50)
        before = [t["id"] for t in s.tabs() if t["active"]]

        s.api(
            "apply-layout",
            focus=False,
            kdl='layout { tab name="bg" { pane purpose="task:bg" command="sh -c \'echo MARKER-BG; read x\'" } }',
        )
        s.settle(200)
        after = [t["id"] for t in s.tabs() if t["active"]]
        check(
            "the view stays on the tab it was on",
            before == after,
            f"was viewing {before}, now viewing {after}",
        )

        built = [p for p in s.panes() if p["purpose"] == "task:bg"]
        check("the layout still built its pane", len(built) == 1, str(s.panes()))
        if built:
            # The whole reason background panes were never seen to be broken:
            # the view used to jump to them, and the jump is what sized them.
            check(
                "a pane in a tab nobody looked at still has a real size",
                built[0]["w"] > 2 and built[0]["h"] > 2,
                f"{built[0]['w']}x{built[0]['h']}",
            )
            check(
                "and its program ran at that size, so its output is readable",
                "MARKER-BG" in s.api("capture", id=built[0]["id"])["text"],
                repr(s.api("capture", id=built[0]["id"])["text"][:120]),
            )


def test_a_pane_lands_beside_the_pane_that_asked_not_beside_focus():
    """The reported bug, in one test.

    Pane 1 is "the agent". The human is in another tab, so the only focused
    pane in the session is theirs. The agent asks for a pane beside *itself*
    by id, and it has to arrive in the agent's tab with nobody's view moving.
    """
    with Session(TALKER, cols=80, rows=24) as s:
        s.settle(100)
        agent = s.pane(0)["id"]
        agent_tab = s.pane(0)["tab_id"]
        s.api("new-tab", name="human-tab")
        s.api("select-tab", index=2)
        s.settle(50)
        human = s.focused()

        s.api(
            "apply-layout",
            focus=False,
            kdl='layout { tab name="work" { pane purpose="task:x" command="sh -c \'echo MARKER-TWO; read x\'" } }',
        )
        s.settle(150)
        new = [p for p in s.panes() if p["purpose"] == "task:x"][0]["id"]

        reply = s.api("move-pane", id=new, tab=agent_tab, beside=agent, focus=False)
        check("the move is accepted", reply.get("ok"), str(reply))
        s.settle(50)

        rows = {p["id"]: p for p in s.panes()}
        check(
            "the pane lands in the tab of the pane that asked for it",
            rows[new]["tab_id"] == agent_tab,
            f"wanted tab {agent_tab}, got {rows[new]['tab_id']}",
        )
        check(
            "focus does not move to it",
            not rows[new]["focused"],
            "the new pane took focus",
        )
        check(
            "the human keeps focus",
            rows[human["id"]]["focused"],
            f"focus went to {[p['id'] for p in s.panes() if p['focused']]}",
        )
        check(
            "and keeps the tab they were looking at",
            [t["id"] for t in s.tabs() if t["active"]] == [human["tab_id"]],
            str(s.tabs()),
        )

        # `beside` has to be in the destination tab, or the caller has the
        # wrong idea of where things are and should hear so.
        bad = s.api("move-pane", id=new, tab=human["tab_id"], beside=agent)
        check("a `beside` in the wrong tab is refused", not bad.get("ok"), str(bad))


def test_a_pane_elsewhere_can_be_read_and_driven_without_focus():
    with Session(TALKER, cols=80, rows=24) as s:
        s.settle(100)
        mine = s.pane(0)["id"]
        s.api("new-tab", name="elsewhere")
        s.api("select-tab", index=2)
        s.settle(50)
        viewing = [t["id"] for t in s.tabs() if t["active"]]

        cap = s.api("capture", id=mine)
        check(
            "capture reads a pane in a tab that is not on screen",
            "MARKER-ONE" in cap["text"],
            repr(cap["text"][:120]),
        )
        check(
            "capture does not change which tab is on screen",
            [t["id"] for t in s.tabs() if t["active"]] == viewing,
            str(s.tabs()),
        )
        check(
            "capture does not move focus",
            s.focused()["tab_id"] != s.pane(0)["tab_id"] or len(s.panes()) == 1,
            str(s.focused()),
        )

        # raw into a named pane: the pane's program is `read x`, so a line ends
        # it -- observable as the pane no longer being alive.
        # raw into a named pane: its program is `read x`, so a line ends it.
        # A real newline in `data`: the JSON form carries bytes, only the
        # bare-verb form unescapes (`raw q\n`).
        s.api("raw", id=mine, data="q\n")

        def ended(_):
            rows = [p for p in s.panes() if p["id"] == mine]
            # The session's own shell pane leaves no husk when it ends, so
            # "gone" and "not alive" are both the line having been delivered.
            return not rows or not rows[0]["alive"]

        s.until(ended)
        check(
            "raw id:<pane> reaches a pane that is not focused",
            ended(None),
            str([p for p in s.panes() if p["id"] == mine]),
        )
        check(
            "and still did not move the view",
            [t["id"] for t in s.tabs() if t["active"]] == viewing,
            str(s.tabs()),
        )

        check(
            "raw at an id that does not exist is refused",
            not s.api("raw", id=99999, data="x").get("ok"),
            "a write to a dead id was accepted",
        )
        check(
            "capture at an id that does not exist is refused",
            not s.api("capture", id=99999).get("ok"),
            "a read of a dead id was accepted",
        )


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
