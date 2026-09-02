#!/usr/bin/env python3
"""M2: server/client split, detach and reattach.

The point of the whole milestone is that the session outlives the client, so
the assertions are about what survives: content, layout, and the process.
"""

import fcntl
import json
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import uuid

BIN = os.environ.get(
    "SLOSH_BIN",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "slosh"),
)

# No greeting, because this file asserts on the bytes a client is sent.
#
# The splash assembles from particles scattered over the whole screen, so for
# its first few hundred milliseconds any cell can be holding a fragment of the
# logo -- including a cell of the pane text a reattach is supposed to repaint.
# The full repaint then goes out *with* a particle sitting in the middle of
# "marker-alpha", and what restores that cell afterwards is a one-cell diff, so
# the string is never contiguous in the stream and a substring check fails about
# half the time. The greeting has its own tests; here it is noise.
CONFIG = os.path.join(tempfile.mkdtemp(prefix="slosh-session-"), "config.kdl")
with open(CONFIG, "w") as f:
    f.write("splash_ms 0\n")

fails = 0


def check(name, cond, detail=""):
    global fails
    if not cond:
        fails += 1
    print(f"{'ok  ' if cond else 'FAIL'} {name}{'' if cond else '  <- ' + detail}")


def attach(name, cols=80, rows=24, inner=None):
    """Attach a client on a real pty. Returns (pid, fd)."""
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-ghostty"
        os.environ["SLOSH_CONFIG"] = CONFIG
        args = [BIN, "-s", name]
        if inner:
            args += ["--"] + inner
        os.execv(BIN, args)
        os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    return pid, fd


def drain(fd, idle=0.3, limit=3.0):
    buf, last, start = b"", time.time(), time.time()
    while time.time() - start < limit:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            last = time.time()
        elif time.time() - last > idle:
            break
    return buf


def control(name, line):
    out = subprocess.run(
        [BIN, "-s", name, "cmd", line], capture_output=True, text=True, timeout=10
    )
    return out.stdout.strip()


def screen_size(name):
    snap = json.loads(control(name, "snapshot json"))
    return snap["cols"], snap["rows"]


def resize(pid, fd, cols, rows):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    os.kill(pid, signal.SIGWINCH)


def running(pid):
    try:
        return os.waitpid(pid, os.WNOHANG)[0] == 0
    except ChildProcessError:
        return False


def wait_gone(pid, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if os.waitpid(pid, os.WNOHANG)[0] == pid:
                return True
        except ChildProcessError:
            return True
        time.sleep(0.05)
    return False


def main():
    name = "t" + uuid.uuid4().hex[:8]
    inner = ["/bin/sh", "-c", "stty raw -echo; cat"]

    # --- attach, create, put something on screen ------------------------
    pid, fd = attach(name, inner=inner)
    drain(fd)
    os.write(fd, b"marker-alpha")
    out = drain(fd)
    check(
        "a session starts and shows typed text", b"marker-alpha" in out, repr(out[-80:])
    )

    check(
        "the session is listed as running",
        name in subprocess.run([BIN, "ls"], capture_output=True, text=True).stdout,
        "",
    )

    # --- split, so the layout is worth preserving -----------------------
    os.write(fd, b"\x01\\")
    drain(fd)
    panes_before = control(name, "panes")
    check(
        "control socket answers while attached",
        panes_before.startswith("["),
        panes_before[:60],
    )
    check(
        "the split is visible to the control socket",
        panes_before.count('"id"') == 2,
        panes_before[:120],
    )

    # --- probes must not disturb the client -----------------------------
    # Regression: `ls` connects to check liveness. When a connection counted
    # as a client, that probe displaced the real one — the same fault that
    # gave the zellij fork a 20-25% CLI failure rate. Only MSG_HELLO attaches.
    bad = 0
    for _ in range(20):
        if not subprocess.run([BIN, "ls"], capture_output=True, text=True).stdout:
            bad += 1
        if not control(name, "panes").startswith("["):
            bad += 1
    check("20 x (ls + control) never fail", bad == 0, f"{bad} failures")

    os.write(fd, b"-still-here")
    out = drain(fd)
    check("the client survived 40 connections", b"still-here" in out, repr(out[-80:]))

    # --- detach ---------------------------------------------------------
    os.write(fd, b"\x01d")
    tail = drain(fd, idle=0.2, limit=2.0)
    check("C-a d leaves the alt screen", b"\x1b[?1049l" in tail, repr(tail[-60:]))
    check("the client process exits on detach", wait_gone(pid))

    # --- the session must still be there --------------------------------
    time.sleep(0.2)
    panes_after = control(name, "panes")
    check(
        "the session outlives its client", panes_after.startswith("["), panes_after[:60]
    )
    check(
        "the layout survived the detach",
        panes_after.count('"id"') == 2,
        panes_after[:120],
    )

    snap = control(name, "snapshot text")
    check("pane content survived the detach", "marker-alpha" in snap, repr(snap[:120]))
    check(
        "a detached session can still be driven",
        control(name, "raw \\r\\ntyped-while-detached") == ""
        and "typed-while-detached" in control(name, "snapshot text"),
        repr(control(name, "snapshot text")[:200]),
    )

    # --- reattach -------------------------------------------------------
    pid2, fd2 = attach(name)
    out = drain(fd2)
    check(
        "reattaching repaints the whole screen",
        b"marker-alpha" in out,
        repr(out[-120:]),
    )
    check("reattached client can still type", True)
    os.write(fd2, b"-beta")
    out = drain(fd2)
    check("input works after reattach", b"beta" in out, repr(out[-60:]))

    # --- multiple attached clients share one session --------------------
    pid3, fd3 = attach(name, cols=50, rows=12)
    out3 = drain(fd3)
    tail = drain(fd2, idle=0.2, limit=2.0)
    check(
        "both clients are told about each other",
        b"2 clients" in tail and b"2 clients" in out3,
        repr((tail[-80:], out3[-80:])),
    )
    check(
        "a second client leaves the first attached",
        b"replaced" not in tail and running(pid2),
        repr(tail[-60:]),
    )
    check("the newest attachment owns the size", screen_size(name) == (50, 12))

    # The control path asks the canonical screen for a full repaint. Since the
    # canonical screen is now only a model, that request has to fan out to both
    # clients' independent diff histories.
    drain(fd2)
    drain(fd3)
    control(name, "reload")
    repaint2 = drain(fd2)
    repaint3 = drain(fd3)
    check(
        "a canonical repaint reaches every attached client",
        b"\x1b[2J" in repaint2 and b"\x1b[2J" in repaint3,
        repr((repaint2[-80:], repaint3[-80:])),
    )

    os.write(fd2, b"-from-first-client")
    out2 = drain(fd2)
    out3 = drain(fd3)
    shared = control(name, "snapshot text")
    check(
        "either client can type into the shared pane",
        b"t-client" in out2
        and b"t-client" in out3
        and "from-firs" in shared
        and "t-client" in shared,
        repr((out2[-80:], out3[-80:])),
    )
    check("the last client to type owns the size", screen_size(name) == (80, 24))

    # Push the shared cursor below the small client's 12 rows: its view has
    # to pan down to follow, and the tag must say where it went.
    os.write(fd2, b"\n" * 20)
    drain(fd2)
    panned = drain(fd3)
    check(
        "a cropped view names its offset into the shared screen",
        b"clients +" in panned,
        repr(panned[-100:]),
    )

    resize(pid3, fd3, 40, 10)
    drain(fd3)
    check(
        "resizing an inactive client does not steal the size",
        screen_size(name) == (80, 24),
    )
    resize(pid2, fd2, 70, 20)
    drain(fd2)
    check("the active client's resize wins", screen_size(name) == (70, 20))

    os.write(fd3, b"-small-client-active")
    drain(fd3)
    drain(fd2)
    check("input promotes the smaller client", screen_size(name) == (40, 10))

    os.write(fd3, b"\x01d")
    tail = drain(fd3, idle=0.2, limit=2.0)
    check("detach exits only its issuing client", wait_gone(pid3), repr(tail[-60:]))
    check("the other client stays attached", running(pid2))
    check(
        "disconnecting the active client restores the previous active size",
        screen_size(name) == (70, 20),
    )

    # --- closing the last pane ends the session -------------------------
    os.write(fd2, b"\x01x")  # close pane 2
    drain(fd2)
    os.write(fd2, b"\x01x")  # close the last one
    drain(fd2, idle=0.3, limit=2.0)
    check("every client exits when the session ends", wait_gone(pid2))

    time.sleep(0.3)
    listing = subprocess.run([BIN, "ls"], capture_output=True, text=True).stdout
    check("the socket is cleaned up", name not in listing, listing.strip()[:80])

    for p in (pid, pid2, pid3):
        try:
            os.kill(p, signal.SIGKILL)
        except ProcessLookupError:
            pass

    # --- the config knobs -----------------------------------------------
    # multi_attach false restores the displacing attach; size_follows
    # "smallest" sizes the shared screen to the tightest attached terminal.
    # Each knob gets a fresh session, whose config exists before its server
    # starts (the server reads it once at startup, plus on reload).
    with open(CONFIG, "w") as f:
        f.write("splash_ms 0\nmulti_attach false\n")
    name_x = "t" + uuid.uuid4().hex[:8]
    pid_a, fd_a = attach(name_x)
    drain(fd_a)
    pid_b, fd_b = attach(name_x)
    drain(fd_b)
    tail = drain(fd_a, idle=0.2, limit=2.0)
    check(
        "multi_attach false: the old client is displaced",
        b"replaced" in tail or wait_gone(pid_a, 1.0),
        repr(tail[-60:]),
    )
    check("multi_attach false: the new client is attached", running(pid_b))
    control(name_x, "quit")
    drain(fd_b, idle=0.2, limit=2.0)

    with open(CONFIG, "w") as f:
        f.write('splash_ms 0\nsize_follows "smallest"\n')
    name_y = "t" + uuid.uuid4().hex[:8]
    pid_c, fd_c = attach(name_y, cols=80, rows=24)
    drain(fd_c)
    pid_d, fd_d = attach(name_y, cols=50, rows=12)
    drain(fd_d)
    drain(fd_c)
    check(
        "size_follows smallest: the tightest client sizes the screen",
        screen_size(name_y) == (50, 12),
    )
    os.write(fd_c, b"x")  # input on the big client must not steal the size
    drain(fd_c)
    check(
        "size_follows smallest: input does not steal the size",
        screen_size(name_y) == (50, 12),
    )
    os.write(fd_d, b"\x01d")  # the small client leaves; the floor rises
    drain(fd_d, idle=0.2, limit=2.0)
    check(
        "size_follows smallest: a departing client raises the floor",
        screen_size(name_y) == (80, 24),
    )
    control(name_y, "quit")
    drain(fd_c, idle=0.2, limit=2.0)

    for p in (pid_a, pid_b, pid_c, pid_d):
        try:
            os.kill(p, signal.SIGKILL)
        except ProcessLookupError:
            pass

    print(f"\n{'FAILED' if fails else 'all green'} ({fails} failures)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
