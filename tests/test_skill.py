#!/usr/bin/env python3
"""`.agents/skills/driving-slosh/SKILL.md`: the agent-facing interface, checked.

A skill is documentation an agent acts on without a human reading it first, which
makes a stale one worse than a missing one: a wrong verb becomes a failed tool call
somebody has to debug, and a verb we removed becomes an agent that cannot do the
thing it was told it could. So every verb, every environment variable and every
`panes` field the skill names is checked against the program here.

It is deliberately not checked for prose. It is checked for claims that can rot.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import BIN, Session, check, report

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKILL = os.path.join(ROOT, ".agents", "skills", "driving-slosh", "SKILL.md")
SH = ["/bin/sh", "-c", "read x"]


def text():
    with open(SKILL) as f:
        return f.read()


def test_the_skill_is_where_agents_look_for_it():
    check("it exists", os.path.exists(SKILL), SKILL)
    body = text()
    check("it opens with YAML frontmatter", body.startswith("---\n"), body[:40])
    fm = body.split("---")[1]
    check("naming itself for its directory", "name: driving-slosh" in fm, fm)
    check("and saying when it applies", "description:" in fm, fm)
    # The description is what a harness matches on, so it has to mention the
    # things a user would say.
    for word in ("slosh", "pane", "workspace", "SLOSH"):
        check("the description mentions " + word, word in fm, fm)


def test_every_verb_it_names_is_a_verb_the_program_has():
    """The check that matters: a skill naming a verb we do not have is a tool call
    that fails in front of somebody."""
    body = text()
    named = set(re.findall(r'\{"cmd":"([a-z-]+)"', body))
    named |= set(re.findall(r"^\| `([a-z-]+)`(?: `([a-z-]+)`)*", body, re.M))
    # the verb table lists several per row, backticked
    for row in re.findall(r"^\| ((?:`[a-z-]+` ?)+)\|", body, re.M):
        named |= set(re.findall(r"`([a-z-]+)`", row))
    named = {v for v in named if isinstance(v, str) and v}
    check("the skill names a useful number of verbs", len(named) > 20, str(named))

    with Session(SH, cols=80, rows=12) as s:
        unknown = []
        for verb in sorted(named):
            if verb == "quit":  # would end the session under test
                continue
            r = s.api(verb)
            if r.get("ok") is False and "unknown cmd" in r.get("error", ""):
                unknown.append(verb)
        check("every verb it names exists", not unknown, "unknown: " + str(unknown))


def test_the_environment_it_promises_is_the_environment_panes_get():
    body = text()
    for var in ("SLOSH", "SLOSH_SESSION", "SLOSH_BIN"):
        check("the skill documents " + var, var in body, "missing " + var)

    # Started from an environment that already names a session and a binary,
    # because that is the case the claim is *about*. This suite normally runs
    # inside a pane, where both are set, so a driver that merely never sets them
    # is indistinguishable from one that clears them -- until it hands its own
    # panes the outer session's name and a script in there sends its commands
    # somewhere else. Passing the lie in makes the difference visible from any
    # shell, rather than only from one that happens to have them set.
    lie = {"SLOSH_SESSION": "somebody-elses-session", "SLOSH_BIN": "/nope/x"}
    probe = [
        "/bin/sh",
        "-c",
        'echo "V=[$SLOSH] S=[$SLOSH_SESSION] B=[$SLOSH_BIN]"; read x',
    ]
    with Session(probe, cols=120, rows=8, env=lie) as s:
        s.until_text("V=[")
        screen = s.snapshot().screen()
        check("a pane really is told it is in one", "V=[1]" in screen, screen)
        check(
            "--script has no socket, so it names no session", "S=[]" in screen, screen
        )
        check(
            "...and clears an inherited one rather than passing it on",
            "somebody-elses-session" not in screen,
            screen,
        )
        check(
            "the binary is the one that made the pane, not an inherited path",
            "/nope/x" not in screen and "slosh" in screen.split("B=[")[1],
            screen,
        )
    flat = " ".join(body.split())  # the file is wrapped; the claim is not
    check(
        "which the skill says out loud",
        "unset under `--script`" in flat,
        "not documented",
    )


def test_a_real_session_names_itself_to_its_panes():
    """The other half, and the reason `--script` clearing it is not just
    `unsetenv` everywhere: a session *does* tell its panes which one it is, and it
    is its own name even when it was started from inside another session."""
    out = os.path.join(tempfile.mkdtemp(prefix="slosh-env-"), "seen")
    name = "envclaim-%d" % os.getpid()
    env = dict(os.environ)
    env["SLOSH_SESSION"] = "somebody-elses-session"
    try:
        subprocess.run(
            [
                BIN,
                "-s",
                name,
                "--",
                "/bin/sh",
                "-c",
                'echo "S=[$SLOSH_SESSION]" > %s; read x' % out,
            ],
            capture_output=True,
            text=True,
            timeout=30,
            input="",
            env=env,
        )
        deadline = time.time() + 5
        seen = ""
        while time.time() < deadline:
            if os.path.exists(out):
                seen = open(out).read().strip()
                if seen:
                    break
            time.sleep(0.05)
        check(
            "a pane in a real session is told which session that is",
            seen == "S=[%s]" % name,
            repr(seen),
        )
    finally:
        subprocess.run(
            [BIN, "-s", name, "cmd", '{"cmd":"quit"}'], capture_output=True, text=True
        )


def test_the_panes_fields_it_tells_agents_to_poll_are_real():
    """The skill's central recipe polls `alive` and `exit_code` off `panes` and
    filters on `purpose`. If any of those three moved, the recipe is a hang."""
    body = text()
    with Session(SH, cols=80, rows=12) as s:
        pane = s.panes()[0]
        for field in (
            "id",
            "purpose",
            "alive",
            "exit_code",
            "tab_id",
            "content_x",
            "content_y",
            "content_w",
            "content_h",
        ):
            check("panes reports " + field, field in pane, str(pane))
            check("and the skill names it", field in body, "missing " + field)


def test_the_recipe_in_the_skill_actually_works():
    """Run the skill's own worked example: a tagged command pane, polled to
    completion, exit status read off `panes`. Copied from the file rather than
    paraphrased, so a change to the file is a change to what is tested."""
    with Session(SH, cols=90, rows=14) as s:
        s.api(
            "apply-layout",
            kdl='layout { tab name="build" '
            '{ pane purpose="task:build" command="sh -c \'echo built; exit 3\'" } }',
        )
        target = None
        for _ in range(200):
            s.settle(30)
            got = [p for p in s.panes() if p["purpose"] == "task:build"]
            if got and not got[0]["alive"]:
                target = got[0]
                break
        check(
            "the tagged pane is findable by purpose", target is not None, str(s.panes())
        )
        check(
            "it reports the real exit status once it stops",
            target and target["exit_code"] == 3,
            str(target),
        )
        check(
            "and its output is still readable afterwards",
            "built" in s.snapshot().screen(),
            s.snapshot().screen(),
        )


def test_it_is_free_of_this_machine():
    """ "Plug and play" is a property somebody has to keep. A path out of the
    author's home directory in a skill is a path an end user cannot use."""
    body = text()
    bad = []
    for pat in (
        r"/home/[a-z]",
        r"/Users/[a-z]",
        r"/tmp/[a-z0-9]",
        r"\bclank\b",
        re.escape(os.path.expanduser("~")),
    ):
        bad += [m.group(0) for m in re.finditer(pat, body)]
    check("no path from the machine it was written on", not bad, str(set(bad)))
    check(
        "it reaches the binary through the environment",
        "SLOSH_BIN" in body and "not on your `PATH`" in body,
        "no PATH note",
    )


def test_settle_is_not_promised_over_the_socket():
    """The skill tells agents to poll, because the thing that would save them from
    polling is headless-only. If that ever changes, the skill is understating what
    the program can do -- which is the good direction, but it should not be
    accidental."""
    with Session(SH, cols=80, rows=10) as s:
        r = s.api("settle", ms=10)
        check("settle is not a socket verb", r.get("ok") is False, str(r))
    check(
        "and the skill says there is no wait primitive",
        "no wait primitive" in text(),
        "not documented",
    )


def test_the_skill_is_reachable_from_the_docs_people_read():
    """An agent finds it by convention. A human has to be told it is there."""
    for path, what in (
        (os.path.join(ROOT, "contrib", "README.md"), "contrib"),
        (os.path.join(ROOT, "docs", "scripting.md"), "scripting"),
    ):
        with open(path) as f:
            body = f.read()
        check("%s points at the skill" % what, "driving-slosh" in body, path)


if __name__ == "__main__":
    for name, fn in sorted(list(globals().items())):
        if name.startswith("test_"):
            fn()
    sys.exit(report())
