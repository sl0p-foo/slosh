#!/usr/bin/env python3
"""A pane's program holds its own terminal and nothing else.

The session's own descriptors -- the listening socket, each attached client's
socket, every *other* pane's pty master -- used to be inherited by whatever ran
in a pane, and the programs started in panes (shells, agents, containers)
outlive the server that started them. So a server that died left its socket
held open by somebody's shell: the address stayed in LISTEN with nobody
accepting, and every later `slosh` blocked in connect() forever, on a cleared
screen, with no message. These are the two halves of that, asserted from
inside a pane: what it inherited, and that a wedged socket is now an error.
"""

import os
import socket
import subprocess
import sys
import tempfile
import time

from harness import BIN, Session, check, report

# `ls -1 /proc/self/fd` from a shell shows the shell's own fds, plus the one
# the pipeline opens for the listing itself. 0,1,2 and one stray is clean;
# anything beyond that is inherited.
LIST_FDS = ["/bin/sh", "-c", "stty raw -echo; ls -1 /proc/self/fd | tr '\\n' ' '; cat"]


def pane_fds(screen_text):
    """The fd numbers a pane printed, as ints."""
    out = set()
    for word in screen_text.split():
        if word.isdigit():
            out.add(int(word))
    return out


def test_a_pane_inherits_nothing_but_its_own_terminal():
    with Session(LIST_FDS, cols=60, rows=8) as s:
        snap = s.until_text("0 1 2")
        fds = pane_fds(snap.screen())
        # 0, 1, 2 are the pty; `ls` itself needs one more to read the directory.
        strays = {f for f in fds if f > 3}
        check(
            "a pane's program has no inherited descriptors",
            not strays,
            f"inherited {sorted(strays)} (screen: {snap.screen()!r})",
        )


def test_a_second_pane_does_not_get_the_first_pane_s_terminal():
    """Two panes, so there is another pty master to inherit -- the leak that
    let one pane's program read and write another pane's terminal."""
    with Session(LIST_FDS, cols=60, rows=12) as s:
        s.settle()
        second = s.api("split", dir="rows", command=" ".join(LIST_FDS[2:]))
        check("the split was made", second.get("ok") is True, str(second))
        snap = s.until(lambda sn: sn.screen().count("0 1 2") >= 2)
        check(
            "neither pane holds a second terminal",
            not {f for f in pane_fds(snap.screen()) if f > 3},
            snap.screen(),
        )


def test_a_listening_socket_nobody_accepts_is_an_error_not_a_hang():
    """The belt: even if something does leave a socket listening with no
    server behind it, attaching says so instead of waiting forever."""
    with tempfile.TemporaryDirectory() as run:
        sock = os.path.join(run, "slosh", "wedged.sock")
        os.makedirs(os.path.dirname(sock), exist_ok=True)
        # A listener that never accepts: exactly the state a dead server's
        # inherited socket left behind.
        lst = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        lst.bind(sock)
        lst.listen(4)
        env = dict(os.environ)
        env["XDG_RUNTIME_DIR"] = run
        env["SLOSH_CONFIG"] = "/nonexistent/slosh.kdl"
        env.pop("SLOSH", None)
        env.pop("SLOSH_SESSION", None)
        started = time.monotonic()
        try:
            p = subprocess.run(
                [BIN, "-s", "wedged", "cmd", '{"cmd":"tabs"}'],
                env=env,
                capture_output=True,
                text=True,
                timeout=30,
            )
        except subprocess.TimeoutExpired:
            check("attaching to a wedged session returns", False, "it hung for 30s")
            lst.close()
            return
        took = time.monotonic() - started
        lst.close()
        check(
            "attaching to a wedged session fails instead of hanging",
            p.returncode != 0,
            f"rc={p.returncode} out={p.stdout!r} err={p.stderr!r}",
        )
        check(
            "and it gives up in seconds, not never",
            took < 20,
            f"took {took:.1f}s",
        )


if __name__ == "__main__":
    test_a_pane_inherits_nothing_but_its_own_terminal()
    test_a_second_pane_does_not_get_the_first_pane_s_terminal()
    test_a_listening_socket_nobody_accepts_is_an_error_not_a_hang()
    sys.exit(report())
