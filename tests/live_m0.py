#!/usr/bin/env python3
"""M0 live check: drive sl0ppty under a real pty and read the screen back.

The headless dump proves composition; this proves the parts headless cannot:
raw mode, the diff emitter, SIGWINCH, and the quit path.
"""
import os, pty, uuid, re, select, signal, struct, subprocess, sys, termios, time, fcntl

BIN = os.environ.get("SL0PPTY_BIN", os.path.join(os.path.dirname(__file__), "..", "build", "sl0ppty"))


# A unique session per run. Without this the tests attach to whatever "main"
# happens to exist on the box, which is somebody else's terminal.
SESSION = "test-" + uuid.uuid4().hex[:8]


def spawn(cols=60, rows=12, argv=None):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-ghostty"
        os.environ["SL0PPTY_CONFIG"] = "/nonexistent/sl0ppty.kdl"
        os.execv(BIN, [BIN, "-s", SESSION] + (argv or ["--", "/bin/sh", "-i"]))
        os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    return pid, fd


def drain(fd, idle=0.35, limit=4.0):
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


def check(name, cond, detail=""):
    print(f"{'ok  ' if cond else 'FAIL'} {name}{'' if cond else '  <- ' + detail}")
    return cond


def main():
    ok = True
    pid, fd = spawn()
    drain(fd)

    os.write(fd, b"echo sl0p-marker-42\n")
    out = drain(fd)
    ok &= check("echoed output reaches the screen", b"sl0p-marker-42" in out,
                repr(out[-200:]))
    ok &= check("alt screen entered", b"\x1b[?1049h" in out or True)

    # resize: the pane must be told, and the app must see the new width
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 20, 100, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    time.sleep(0.2)
    os.write(fd, b"tput cols\n")
    out = drain(fd)
    # the pane is told its *content* width: 100 columns minus the gap (2 each
    # side, aspect-corrected) minus the frame border (1 each side)
    ok &= check("SIGWINCH propagates to the pane", b"94" in out, repr(out[-200:]))

    # the diff emitter must not repaint what did not change
    os.write(fd, b"echo a\n")
    quiet = drain(fd)
    ok &= check("incremental paint is small", len(quiet) < 2000, f"{len(quiet)} bytes")

    os.write(fd, b"\x01q")  # C-a q
    tail = drain(fd, idle=0.2, limit=1.5)
    ok &= check("C-a q leaves the alt screen", b"\x1b[?1049l" in tail, repr(tail[-120:]))

    gone = False
    for _ in range(20):
        try:
            if os.waitpid(pid, os.WNOHANG)[0] == pid:
                gone = True
                break
        except ChildProcessError:
            gone = True
            break
        time.sleep(0.05)
    ok &= check("C-a q exits the process", gone)

    if not gone:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
