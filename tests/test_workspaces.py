"""Workspaces: projects on disk, and the tabs they open as.

A *project* is a directory somebody works in -- one with a `slosh.layout`
saying what it needs, or a `.git` and nothing said yet. A *workspace* is the tab
it occupies here, and membership is that tab's `purpose` in the `project:`
namespace, so there is no second answer to "what is this tab" and a dumped
session restores membership for free.

The list is derived when it is asked for rather than watched: an answer nobody
remembered cannot be stale.
"""

import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import BIN, Session, check, report

SH = ["/bin/sh", "-c", "read x"]

API_LAYOUT = """layout {
    tab name="api" {
        pane purpose="agent:main"
        pane split="rows" {
            pane cwd="src" command="echo dev-server" suspended=true
            pane purpose="shell:scratch"
        }
    }
}
"""


def roots(**projects):
    """A project root holding the projects described.

    Values: layout text (a declared project), "git" (a `.git` and nothing else),
    or None (a directory that is neither, which must not be listed).
    """
    base = os.path.realpath(tempfile.mkdtemp(prefix="slosh-ws-"))
    dev = os.path.join(base, "dev")
    for name, what in projects.items():
        d = os.path.join(dev, *name.split("__"))
        os.makedirs(os.path.join(d, "src"), exist_ok=True)
        if what == "git":
            os.makedirs(os.path.join(d, ".git"), exist_ok=True)
        elif what:
            with open(os.path.join(d, "slosh.layout"), "w") as f:
                f.write(what)
    cfg = os.path.join(base, "config.kdl")
    with open(cfg, "w") as f:
        f.write('project_roots "%s" depth=2\n' % dev)
    return base, dev, cfg


def session(cfg, argv=None, **kw):
    """A session whose panes run `argv`.

    The default is the suite's `read x` stub, which is right for everything that
    only cares about shape. A test that *types a command* needs a real shell:
    `read x` would swallow the line and exit, taking the pane with it."""
    kw.setdefault("cols", 100)
    kw.setdefault("rows", 30)
    return Session(argv or SH, config=cfg, **kw)


# ---- discovery -------------------------------------------------------------


def test_the_old_layout_spelling_is_still_found_never_preferred():
    """`slosh.layout.kdl` was the name before the extension collided with
    zellij's. A checked-in workspace must not vanish on upgrade, so the scan
    still finds the old spelling -- and where both exist, the new one wins,
    because two files disagreeing needs one answer."""
    base, dev, cfg = roots(old=None, both=None)
    with open(os.path.join(dev, "old", "slosh.layout.kdl"), "w") as f:
        f.write(API_LAYOUT)
    with open(os.path.join(dev, "both", "slosh.layout"), "w") as f:
        f.write(API_LAYOUT)
    with open(os.path.join(dev, "both", "slosh.layout.kdl"), "w") as f:
        f.write(API_LAYOUT)
    with session(cfg) as s:
        got = {w["name"]: w for w in s.api("workspaces")["workspaces"]}
        check(
            "a project with only the old name is still a project",
            "old" in got and got["old"]["layout"].endswith("slosh.layout.kdl"),
            str(got.get("old")),
        )
        check(
            "where both spellings exist the new one wins",
            "both" in got and got["both"]["layout"].endswith("/slosh.layout"),
            str(got.get("both")),
        )


def test_a_project_is_a_layout_file_or_a_git():
    """Both, because `~/dev` with forty checkouts and three layout files would
    otherwise be a picker with three rows -- and a `.git` is exactly the project
    that has not said what it needs yet."""
    base, dev, cfg = roots(api=API_LAYOUT, web="git", notes=None)
    with session(cfg) as s:
        got = {w["name"]: w for w in s.api("workspaces")["workspaces"]}
        check("a directory with a layout file is a project", "api" in got, str(got))
        check("and one with a .git is too", "web" in got, str(got))
        check("a directory that is neither is not listed", "notes" not in got, str(got))
        check(
            "the declared one names its file",
            got["api"]["layout"].endswith("slosh.layout"),
            str(got["api"]),
        )
        check(
            "the inferred one has no file to name",
            got["web"]["layout"] == "",
            str(got["web"]),
        )
        check(
            "and it reports the file's mtime, so drift is derivable outside",
            got["api"]["mtime"] > 0 and got["web"]["mtime"] == 0,
            str(got),
        )


def test_it_looks_below_the_root_but_never_inside_a_project():
    """`~/dev/work/api` is as common as `~/dev/api`. A directory that *is* a
    project is not descended into, which is what keeps this off node_modules
    without a rule about node_modules."""
    base, dev, cfg = roots(api=API_LAYOUT, work__gateway="git")
    # A checkout with its own subdirectories, one of which looks like a project.
    inner = os.path.join(dev, "api", "vendor", "thing")
    os.makedirs(os.path.join(inner, ".git"))
    with session(cfg) as s:
        names = [w["name"] for w in s.api("workspaces")["workspaces"]]
        check("a project two levels down is found", "gateway" in names, str(names))
        check("a project inside a project is not", "thing" not in names, str(names))
        check("they come back sorted", names == sorted(names), str(names))


def test_two_projects_of_the_same_name_are_two_workspaces():
    """The slug hashes the *path*, not the name, so two worktrees of one repo are
    two workspaces -- which is what they are."""
    base, dev, cfg = roots(api="git", hotfix__api="git")
    with session(cfg) as s:
        ws = s.api("workspaces")["workspaces"]
        slugs = {w["purpose"] for w in ws}
        check("both are listed", len(ws) == 2, str(ws))
        check("with different identities", len(slugs) == 2, str(slugs))
        check(
            "and both are named for their directory",
            all(w["purpose"].startswith("project:api.") for w in ws),
            str(slugs),
        )


def test_saying_nothing_about_roots_is_said_out_loud():
    """ "You have no projects" and "you never said where they are" are different
    facts, and answering both with an empty list makes the feature look broken."""
    with Session(SH) as s:
        r = s.api("workspaces")
        check("the list is empty", r["workspaces"] == [], str(r))
        check("and it says why", r["roots"] is False, str(r))
        bad = s.api("open-workspace", name="api")
        check(
            "opening says so too",
            bad["ok"] is False and "project_roots" in bad["error"],
            str(bad),
        )


def test_a_path_outside_every_root_is_refused():
    """Which is also the containment check: a directory no root holds is one this
    session will not open and will not write a layout into."""
    base, dev, cfg = roots(api=API_LAYOUT)
    with session(cfg) as s:
        r = s.api("open-workspace", path="/tmp")
        check("a path no root holds is not a project", r["ok"] is False, str(r))


# ---- opening ---------------------------------------------------------------


def test_opening_a_project_builds_its_layout_and_tags_the_tab():
    base, dev, cfg = roots(api=API_LAYOUT)
    with session(cfg) as s:
        r = s.api("open-workspace", name="api")
        check("it says which tab you landed in", r["tab"] > 0, str(r))
        check("and that it made one", r["created"] is True, str(r))
        check(
            "the tab carries the workspace's identity",
            r["purpose"].startswith("project:api."),
            str(r),
        )

        tab = [t for t in s.tabs() if t["id"] == r["tab"]][0]
        check("the tab is named for the project", tab["name"] == "api", str(tab))
        check(
            "and its purpose is declared, so no program can relabel it",
            tab["purpose"] == r["purpose"] and tab["purpose_declared"] is True,
            str(tab),
        )

        panes = [p for p in s.panes() if p["tab_id"] == r["tab"]]
        check("the layout's panes are there", len(panes) == 3, str(panes))
        tags = {p["purpose"] for p in panes}
        check(
            "carrying the tags the project declared",
            {"agent:main", "shell:scratch"} <= tags,
            str(tags),
        )
        check(
            "and the expensive one has not started",
            sum(1 for p in panes if p["suspended"]) == 1,
            str(panes),
        )


def test_opening_the_same_project_twice_is_one_workspace():
    """`sl0ppi up`'s idempotency, carried over: a script can drive this in a loop
    without asking first, and so can a keystroke."""
    base, dev, cfg = roots(api=API_LAYOUT)
    with session(cfg) as s:
        first = s.api("open-workspace", name="api")
        before = len(s.tabs())
        again = s.api("open-workspace", name="api")
        check("the same tab comes back", again["tab"] == first["tab"], str(again))
        check("and it says it did not make one", again["created"] is False, str(again))
        check("no second tab appeared", len(s.tabs()) == before, str(s.tabs()))


def test_a_project_with_no_layout_opens_as_your_project_layout():
    """One file is the shape you start every checkout in, and its relative paths
    bind to whichever project is opening -- not to the directory the file lives
    in, which would open every project in ~/.config."""
    base, dev, cfg = roots(web="git")
    shared = os.path.join(base, "project.layout")
    with open(shared, "w") as f:
        f.write('layout { tab { pane purpose="agent:main"; pane cwd="src" } }\n')
    with open(cfg, "a") as f:
        f.write('project_layout "%s"\n' % shared)
    with session(cfg) as s:
        r = s.api("open-workspace", name="web")
        panes = [p for p in s.panes() if p["tab_id"] == r["tab"]]
        check("the shared layout was used", len(panes) == 2, str(panes))
        check(
            "and its tags came with it",
            any(p["purpose"] == "agent:main" for p in panes),
            str(panes),
        )

        kdl = s.api("dump-layout", tab=r["tab"])["kdl"]
        want = os.path.join(dev, "web")
        check(
            "its panes are in the project, not beside the shared file",
            kdl.count(want) == 2,
            kdl + " want " + want,
        )


def test_a_project_with_nothing_at_all_opens_as_one_shell_in_it():
    base, dev, cfg = roots(web="git")
    with session(cfg) as s:
        r = s.api("open-workspace", name="web")
        panes = [p for p in s.panes() if p["tab_id"] == r["tab"]]
        check("one pane", len(panes) == 1, str(panes))
        kdl = s.api("dump-layout", tab=r["tab"])["kdl"]
        check("in the project's directory", os.path.join(dev, "web") in kdl, kdl)


def test_opening_suspended_starts_nothing():
    """The "open ten projects, run zero processes" case, which is a different
    question from the one a project's layout answered about its own panes."""
    base, dev, cfg = roots(api=API_LAYOUT)
    with session(cfg) as s:
        r = s.api("open-workspace", name="api", suspended=True)
        panes = [p for p in s.panes() if p["tab_id"] == r["tab"]]
        check(
            "every pane is laid out and asleep",
            all(p["suspended"] for p in panes) and len(panes) == 3,
            str(panes),
        )


def test_a_tab_that_declared_its_own_purpose_keeps_it():
    """Overwriting a declared purpose is the one thing D8 forbids, so such a tab is
    honoured and is simply not a member -- and the reply counts it rather than
    leaving it to be a surprise later."""
    base, dev, cfg = roots(
        api="layout {\n"
        '    tab name="a" { pane }\n'
        '    tab name="b" purpose="notes" { pane }\n'
        "}\n"
    )
    with session(cfg) as s:
        r = s.api("open-workspace", name="api")
        check("one tab was adopted", r["tabs"] == 1, str(r))
        check("and one was left alone", r["honoured"] == 1, str(r))
        purposes = {t["name"]: t["purpose"] for t in s.tabs()}
        check(
            "the declared purpose survived", purposes.get("b") == "notes", str(purposes)
        )


def test_closing_a_workspace_closes_its_tabs():
    base, dev, cfg = roots(
        api='layout {\n    tab name="one" { pane }\n    tab name="two" { pane }\n}\n'
    )
    with session(cfg) as s:
        s.api("open-workspace", name="api")
        before = len(s.tabs())
        r = s.api("close-workspace", name="api")
        check("both of its tabs went", r["closed"] == 2, str(r))
        check("and nothing else did", len(s.tabs()) == before - 2, str(s.tabs()))


# ---- saving ----------------------------------------------------------------


def test_saving_a_tab_writes_a_portable_layout_file():
    """The whole of onboarding: arrange a tab in a checkout, press one key, and the
    project owns a layout that works on any machine."""
    base, dev, cfg = roots(web="git")
    with session(cfg) as s:
        r = s.api("open-workspace", name="web")
        s.key("\\\\")  # split into columns
        s.key("P")  # ...and tag the new pane
        s.send("service:web\\r")
        saved = s.api("save-workspace")
        check(
            "it says where it wrote",
            saved["path"] == os.path.join(dev, "web", "slosh.layout"),
            str(saved),
        )
        check(
            "and that there was nothing there before",
            saved["replaced"] is False,
            str(saved),
        )

        text = open(saved["path"]).read()
        check(
            "every directory is relative to the project",
            'cwd="."' in text and dev not in text,
            text,
        )
        check("the tag came with it", 'purpose="service:web"' in text, text)
        check("the derived workspace purpose did not", "project:" not in text, text)
        check(
            "and it lints as a layout",
            subprocess.run(
                [BIN, "--check", saved["path"]], capture_output=True
            ).returncode
            == 0,
            text,
        )


def test_saving_adopts_an_ad_hoc_tab():
    """The other door in: you were already working in a checkout in a plain tab,
    and saving makes that tab the project's workspace."""
    base, dev, cfg = roots(web="git")
    with session(cfg) as s:
        r = s.api("save-workspace", path=os.path.join(dev, "web"))
        check(
            "it wrote the project's file",
            r["path"].startswith(os.path.join(dev, "web")),
            str(r),
        )
        check(
            "and the tab now carries the workspace",
            r["purpose"].startswith("project:web."),
            str(r),
        )
        listed = {w["name"]: w for w in s.api("workspaces")["workspaces"]}
        check("so the picker shows it open", listed["web"]["tab"] > 0, str(listed))
        check("and it has a layout now", listed["web"]["layout"] != "", str(listed))


def test_saving_will_not_quietly_replace_a_checked_in_file():
    base, dev, cfg = roots(api=API_LAYOUT)
    with session(cfg) as s:
        s.api("open-workspace", name="api")
        first = s.api("save-workspace")
        check(
            "a project that has a layout is refused", first["ok"] is False, str(first)
        )
        check("and told how to mean it", "force" in first["error"], str(first))
        forced = s.api("save-workspace", force=True)
        check("with force it writes", forced["ok"] is True, str(forced))
        check("and says it replaced something", forced["replaced"] is True, str(forced))


def test_a_saved_project_defaults_to_suspending_its_commands():
    """A dump of a session is honest about what is running. A project's layout must
    not be: the pane running this morning's dev server would start one on every
    open."""
    base, dev, cfg = roots(web="git")
    with session(cfg) as s:
        s.api("open-workspace", name="web")
        s.api("split", dir="rows")
        # a pane that was given a command, the way a layout would give one
        s.api("apply-layout", kdl='layout { tab name="t" { pane command="sleep 60" } }')
        t = [x for x in s.tabs() if x["name"] == "t"][0]
        r = s.api("save-workspace", path=os.path.join(dev, "web"), tab=t["id"])
        check("the pane with a command is written asleep", r["suspended"] == 1, str(r))
        text = open(r["path"]).read()
        check(
            "so opening it will not start a dev server", "suspended=true" in text, text
        )


def test_setting_a_project_up_by_hand_and_writing_it_down():
    """The flow the feature exists for: split, start the things, tag them, save.
    Before commands were captured this wrote bare shells, so arranging a project
    and recording it were two separate jobs."""
    base, dev, cfg = roots(web="git")
    with session(cfg, argv=["/bin/sh"]) as s:
        s.api("open-workspace", name="web")
        # a dev server and a log tailer, started the way anybody starts them
        s.raw("sleep 300\\r")
        s.key("P")
        s.send("service:web\\r")
        s.key("-")
        s.raw("tail -f /dev/null\\r")
        s.key("P")
        s.send("logs:web\\r")
        s.settle(150)

        saved = s.api("save-workspace")
        text = open(saved["path"]).read()
        check("the dev server is in the file", 'command="sleep 300"' in text, text)
        check("so is the tailer", 'command="tail -f /dev/null"' in text, text)
        check(
            "both are tagged",
            'purpose="service:web"' in text and 'purpose="logs:web"' in text,
            text,
        )
        check(
            "and both are written asleep, so tomorrow lays them out",
            text.count("suspended=true") == 2,
            text,
        )
        check("counted in the reply", saved["suspended"] == 2, str(saved))
        check(
            "it still lints",
            subprocess.run(
                [BIN, "--check", saved["path"]], capture_output=True
            ).returncode
            == 0,
            text,
        )

    # ...and opening it tomorrow gives the shape with nothing running.
    with session(cfg, argv=["/bin/sh"]) as s2:
        r = s2.api("open-workspace", name="web")
        panes = [p for p in s2.panes() if p["tab_id"] == r["tab"]]
        check("the panes come back", len(panes) == 2, str(panes))
        check(
            "carrying their tags",
            {p["purpose"] for p in panes} == {"service:web", "logs:web"},
            str(panes),
        )
        check(
            "laid out and not running", all(p["suspended"] for p in panes), str(panes)
        )
        s2.api("rerun", id=panes[0]["id"])
        s2.settle(150)
        again = s2.api("dump-layout", tab=r["tab"])["kdl"]
        check(
            "and starting one runs what was saved, not a shell",
            'command="sleep 300"' in again,
            again,
        )


def test_a_saved_workspace_round_trips():
    """Save, close, open: the same shape, the same tags, the same suspension. The
    property that makes onboarding worth doing at all."""
    base, dev, cfg = roots(web="git")
    with session(cfg) as s:
        first = s.api("open-workspace", name="web")
        s.key("\\\\")
        s.key("P")
        s.send("service:web\\r")
        s.key("-")
        s.key("P")
        s.send("shell:scratch\\r")
        saved = s.api("save-workspace")
        before = s.api(
            "dump-layout",
            tab=first["tab"],
            relative_to=os.path.join(dev, "web"),
            suspend="commands",
        )["kdl"]
        s.api("close-workspace", name="web")

        again = s.api("open-workspace", name="web")
        after = s.api(
            "dump-layout",
            tab=again["tab"],
            relative_to=os.path.join(dev, "web"),
            suspend="commands",
        )["kdl"]
        # The reopened tab carries the workspace purpose the file does not name.
        before_body = re.sub(r' purpose="project:[^"]*"', "", before)
        after_body = re.sub(r' purpose="project:[^"]*"', "", after)
        check(
            "what was saved is what comes back",
            before_body == after_body,
            before_body + "\n--- became ---\n" + after_body,
        )
        check("and it says it built it", again["created"] is True, str(again))


def bare1(s, line):
    """A bare verb whose answer is one line.

    The driver writes the reply then a single newline, so a one-line answer is
    one line and nothing follows it. Reading for a terminator that is not coming
    hangs, which is how this was learnt."""
    s._cmd(line)
    return s.proc.stdout.readline().rstrip()


def bare_list(s, line):
    """A bare verb whose answer is a list.

    Its rows each end in a newline, so the driver's own newline lands as a blank
    line after the last one -- the same framing `dump-layout` and
    `snapshot text` have always had."""
    s._cmd(line)
    out = []
    while True:
        got = s.proc.stdout.readline()
        if not got or not got.strip():
            return "\n".join(out)
        out.append(got.rstrip())


def test_every_verb_has_a_bare_form():
    """The human/harness alias runs the same functions, so a script at a shell and
    a script sending JSON cannot drift from each other."""
    base, dev, cfg = roots(api=API_LAYOUT, web="git")
    with session(cfg) as s:
        listed = bare_list(s, "workspaces")
        check(
            "workspaces lists every project with its marker",
            "api" in listed and "layout" in listed and ".git" in listed,
            repr(listed),
        )

        check(
            "open-workspace says what it did",
            "opened tab" in bare1(s, "open-workspace api"),
            "open",
        )
        check(
            "and it really opened one",
            any(t["name"] == "api" for t in s.tabs()),
            str(s.tabs()),
        )
        check(
            "asking again focuses rather than building",
            "focused tab" in bare1(s, "open-workspace api"),
            "again",
        )

        # Saving `web` from `api`'s tab would leave one tab named for one project
        # and carrying the other's purpose. Refused, not obeyed.
        wrong = bare1(s, "save-workspace " + os.path.join(dev, "web"))
        check(
            "a workspace tab cannot be saved into another project",
            "another project's workspace" in wrong,
            repr(wrong),
        )

        # ...and `web`, whose own tab is not open, is saved by naming its tab.
        bare1(s, "open-workspace web")
        wrote = bare1(s, "save-workspace")
        check(
            "save-workspace names the file it wrote",
            "slosh.layout" in wrote,
            repr(wrote),
        )
        check(
            "and it wrote the project it was in",
            os.path.join(dev, "web") in wrote,
            repr(wrote),
        )

        check(
            "close-workspace counts what went",
            "closed 1 tab" in bare1(s, "close-workspace api"),
            "close",
        )
        check(
            "and the tab is gone",
            not any(t["name"] == "api" for t in s.tabs()),
            str(s.tabs()),
        )

        check(
            "a project nobody has says so",
            "no project" in bare1(s, "open-workspace nope"),
            "missing",
        )


def test_a_dumped_session_restores_workspace_membership():
    """Membership is a tab purpose, and a dump already writes those -- so this
    costs nothing and had to be checked rather than assumed."""
    base, dev, cfg = roots(api=API_LAYOUT)
    with session(cfg) as s:
        r = s.api("open-workspace", name="api")
        whole = s.api("dump-layout")["kdl"]
    restored = os.path.join(base, "session.layout")
    with open(restored, "w") as f:
        f.write(whole)
    with Session(SH, config=cfg, layout=restored, cols=100, rows=30) as s2:
        listed = {w["name"]: w for w in s2.api("workspaces")["workspaces"]}
        check(
            "the restored session knows the workspace is open",
            listed["api"]["tab"] > 0,
            str(listed),
        )


# ---- the picker ------------------------------------------------------------


def test_the_picker_lists_projects_and_opens_one():
    base, dev, cfg = roots(api=API_LAYOUT, web="git")
    with session(cfg, cols=96, rows=24) as s:
        s.key("w")
        screen = s.snapshot().screen()
        check("it is titled as projects", "projects" in screen, screen)
        check("a declared project shows its file", "slosh.layout" in screen, screen)
        check("and one without says so", "no layout" in screen, screen)

        s.send("web")
        screen = s.snapshot().screen()
        check("typing narrows it", "web" in screen and "api" not in screen, screen)
        s.send("\\r")
        tabs = s.tabs()
        check("enter opens it", any(t["name"] == "web" for t in tabs), str(tabs))
        check(
            "the picker is gone",
            "projects" not in s.snapshot().screen(),
            s.snapshot().screen(),
        )


def test_the_picker_says_when_there_is_nowhere_to_look():
    with Session(SH, cols=90, rows=20) as s:
        s.key("w")
        screen = s.snapshot().screen()
        check("it does not open an empty list", "projects" not in screen, screen)
        check("it says what to set", "project_roots" in screen, screen)


def test_the_actions_are_in_the_palette():
    base, dev, cfg = roots(api=API_LAYOUT)
    with session(cfg) as s:
        s.key("p")
        s.send("project")
        screen = s.snapshot().screen()
        check("going to one is listed", "go to a project" in screen, screen)
        check("and saving one is too", "save this tab as a layout" in screen, screen)


def test_saving_from_the_keyboard_says_what_it_did():
    base, dev, cfg = roots(web="git")
    with session(cfg) as s:
        s.api("open-workspace", name="web")
        s.key("W")
        screen = s.snapshot().screen()
        check("the toast names the file", "slosh.layout" in screen, screen)
        check("and counts what it wrote", "1 pane" in screen, screen)
        check(
            "the file is there",
            os.path.exists(os.path.join(dev, "web", "slosh.layout")),
            screen,
        )


if __name__ == "__main__":
    for name, fn in sorted(list(globals().items())):
        if name.startswith("test_"):
            fn()
    sys.exit(report())
