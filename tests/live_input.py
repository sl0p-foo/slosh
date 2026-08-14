#!/usr/bin/env python3
"""The interactive client's input path, which headless cannot cover.

The encoding matrix (kitty/legacy/mouse/paste, per pane) lives in
tests/test_screen.py, where it runs deterministically through the headless
driver. What is left here is the part that only exists with a real tty: raw
mode, reading stdin, and the prefix key.
"""
import os, pty, uuid, select, struct, sys, termios, time, fcntl, signal

BIN = os.path.join(os.path.dirname(__file__), "..", "build", "sl0ppty")
INNER = ["--", "/bin/sh", "-c", "stty raw -echo; cat -v"]

fails = 0


def check(name, cond, detail=""):
    global fails
    if not cond:
        fails += 1
    print(f"{'ok  ' if cond else 'FAIL'} {name}{'' if cond else '  <- ' + detail}")


def drain(fd, idle=0.25, limit=2.0):
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


def main():
    session = "test-" + uuid.uuid4().hex[:8]
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-ghostty"
        os.environ["SL0PPTY_CONFIG"] = "/nonexistent/sl0ppty.kdl"
        os.execv(BIN, [BIN, "-s", session] + INNER)
        os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 10, 80, 0, 0))
    drain(fd)

    os.write(fd, b"\x1b[1;5A")
    out = drain(fd).decode("utf-8", "replace")
    check("a key typed at a real tty reaches the pane", "^[[1;5A" in out, repr(out[-60:]))

    os.write(fd, b"\x01g")  # prefix + a key that is bound to nothing
    out = drain(fd)
    check("prefix + unbound key is swallowed", out == b"", repr(out[-60:]))

    os.write(fd, b"\x01\x01")  # prefix + prefix = literal
    out = drain(fd).decode("utf-8", "replace")
    check("C-a C-a sends a literal ^A", "^A" in out, repr(out[-60:]))

    os.write(fd, b"\x01q")
    drain(fd, idle=0.2, limit=1.0)
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except (ProcessLookupError, ChildProcessError):
        pass

    print(f"\n{'FAILED' if fails else 'all green'} ({fails} failures)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
