#!/usr/bin/env python3
"""Running slosh from inside slosh.

A new client displaces the current display, so `slosh` typed into a pane of
the very session it would attach used to displace the client showing that
pane — you watched your own session detach, from inside it. The pane's
environment (SLOSH, SLOSH_SESSION) says which session this is, so the attach
path refuses that one name and only that one name: any other is deliberate
nesting, and the control verbs (`cmd`, `ls`) are how a program in a pane is
*supposed* to reach its own session, so they are untouched.
"""

import os
import shutil
import subprocess
import sys
import tempfile

from harness import BIN, check, report


def run(args, session=None, timeout=10, **env_extra):
    """Run the binary with a clean pane-environment: no inherited SLOSH_*,
    sockets in a private runtime dir, stdin not a tty."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("SLOSH")}
    env.update(env_extra)
    if session is not None:
        env["SLOSH"] = "1"
        env["SLOSH_SESSION"] = session
    return subprocess.run(
        [BIN] + args,
        capture_output=True,
        text=True,
        env=env,
        stdin=subprocess.DEVNULL,
        timeout=timeout,
    )


def test_attaching_to_your_own_session_is_refused():
    rt = tempfile.mkdtemp()
    try:
        r = run(["-s", "mine"], session="mine", XDG_RUNTIME_DIR=rt)
        check("it refuses", r.returncode == 1, str(r.returncode))
        check("and names the session", "inside session mine" in r.stderr, r.stderr)
        check("and offers the detach key", "C-a d" in r.stderr, r.stderr)
        socks = (
            os.listdir(os.path.join(rt, "slosh"))
            if os.path.isdir(os.path.join(rt, "slosh"))
            else []
        )
        check("no session was spawned", not socks, str(socks))
    finally:
        shutil.rmtree(rt, ignore_errors=True)


def test_any_other_session_is_deliberate_nesting():
    rt = tempfile.mkdtemp()
    try:
        # Asked for a *different* session from inside a pane: the guard steps
        # aside. Without a tty the attach then fails on its own terms, but the
        # session was created — which is the proof the refusal did not fire.
        r = run(["-s", "nest"], session="outer", XDG_RUNTIME_DIR=rt)
        check("no refusal", "already inside" not in r.stderr, r.stderr)

        # The control verbs from inside the *same* session: the documented
        # interface for a program in a pane, and exactly what must keep working.
        r = run(["-s", "nest", "cmd", "alive"], session="nest", XDG_RUNTIME_DIR=rt)
        check("cmd still answers", r.returncode == 0 and "true" in r.stdout, str(r))
        r = run(["ls"], session="nest", XDG_RUNTIME_DIR=rt)
        check("ls still answers", r.returncode == 0 and "nest" in r.stdout, str(r))

        # A --script pane leaves SLOSH_SESSION empty: there is nothing this
        # pane is inside, so nothing to refuse.
        r = run(["-s", "nest"], session="", XDG_RUNTIME_DIR=rt)
        check(
            "an empty name refuses nothing", "already inside" not in r.stderr, r.stderr
        )
    finally:
        run(["-s", "nest", "cmd", "quit"], XDG_RUNTIME_DIR=rt)
        shutil.rmtree(rt, ignore_errors=True)


if __name__ == "__main__":
    test_attaching_to_your_own_session_is_refused()
    test_any_other_session_is_deliberate_nesting()
    sys.exit(report())
