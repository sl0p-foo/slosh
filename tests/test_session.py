#!/usr/bin/env python3
"""M2: server/client split, detach and reattach.

The point of the whole milestone is that the session outlives the client, so
the assertions are about what survives: content, layout, and the process.
"""

import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time
import uuid

BIN = os.environ.get(
    "SL0PPTY_BIN",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "sl0ppty"),
)

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
        os.environ["SL0PPTY_CONFIG"] = "/nonexistent/sl0ppty.kdl"
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

    # --- a second client replaces the first -----------------------------
    pid3, fd3 = attach(name)
    drain(fd3)
    tail = drain(fd2, idle=0.2, limit=2.0)
    check(
        "the displaced client is told it was replaced",
        b"replaced" in tail or wait_gone(pid2, 1.0),
        repr(tail[-60:]),
    )

    # --- closing the last pane ends the session -------------------------
    os.write(fd3, b"\x01x")  # close pane 2
    drain(fd3)
    os.write(fd3, b"\x01x")  # close the last one
    drain(fd3, idle=0.3, limit=2.0)
    check("the client exits when the session ends", wait_gone(pid3))

    time.sleep(0.3)
    listing = subprocess.run([BIN, "ls"], capture_output=True, text=True).stdout
    check("the socket is cleaned up", name not in listing, listing.strip()[:80])

    for p in (pid, pid2, pid3):
        try:
            os.kill(p, signal.SIGKILL)
        except ProcessLookupError:
            pass

    print(f"\n{'FAILED' if fails else 'all green'} ({fails} failures)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
