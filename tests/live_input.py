#!/usr/bin/env python3
"""Live check of decode -> re-encode.

The pane runs `cat -v`, which prints control bytes visibly, so whatever the
inner app *actually receives* ends up on our screen. That is the only honest
way to test that we re-encode for the pane rather than forwarding bytes.
"""
import os, pty, select, struct, sys, termios, time, fcntl, signal

BIN = os.path.join(os.path.dirname(__file__), "..", "build", "sl0ptty")
INNER = ["--", "/bin/sh", "-c", "stty raw -echo; cat -v"]


def spawn(cols=80, rows=10, inner=None):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-ghostty"
        os.execv(BIN, [BIN] + (inner or INNER))
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


fails = 0


def check(name, cond, detail=""):
    global fails
    if not cond:
        fails += 1
    print(f"{'ok  ' if cond else 'FAIL'} {name}{'' if cond else '  <- ' + detail}")


def main():
    pid, fd = spawn()
    drain(fd)

    cases = [
        # name, what the outer terminal sends us, what the pane must receive
        ("legacy ctrl-up survives", b"\x1b[1;5A", "^[[1;5A"),
        # ctrl-a is the prefix, so ctrl-b is the honest test of a kitty ctrl key
        ("kitty ctrl-b -> legacy ^B", b"\x1b[98;5u", "^B"),
        ("kitty plain 'z' -> 'z'", b"\x1b[122u", "z"),
        # each case must leave the prefix state clean for the next one
        ("prefix + unbound key is swallowed", b"\x01x", ""),
        ("C-a C-a sends a literal ^A", b"\x01\x01", "^A"),
        ("kitty enter -> \\r", b"\x1b[13u", "^M"),
        ("alt-b -> ESC b", b"\x1bb", "^[b"),
        ("utf8 passes through", "é".encode(), "M-CM-)"),
        ("paste is unwrapped for a plain app", b"\x1b[200~xy\x1b[201~", "xy"),
    ]

    for name, send, want in cases:
        os.write(fd, send)
        out = drain(fd, idle=0.25, limit=1.5)
        text = out.decode("utf-8", "replace")
        if want == "":
            # nothing may reach the pane: no repaint at all
            check(name, out == b"", repr(text[-80:]))
        else:
            check(name, want in text, repr(text[-80:]))

    # A key release must not reach an app that never asked for one.
    os.write(fd, b"\x1b[97;1:3u")
    out = drain(fd, idle=0.25, limit=1.0).decode("utf-8", "replace")
    check("key release is not sent to a legacy app", "a" not in out.replace("\x1b", ""),
          repr(out[-80:]))

    # Mouse events must not leak into an app with no mouse tracking on.
    os.write(fd, b"\x1b[<0;5;5M\x1b[<0;5;5m")
    out = drain(fd, idle=0.25, limit=1.0).decode("utf-8", "replace")
    check("mouse does not leak into a non-mouse app", "[<" not in out, repr(out[-80:]))

    os.write(fd, b"\x01q")
    drain(fd, idle=0.2, limit=1.0)
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except (ProcessLookupError, ChildProcessError):
        pass

    # --- the other direction: a pane that negotiates modern protocols -----
    # Same keys, same client; only the pane's own state differs. Passing here
    # *and* above means we encode per pane rather than forwarding bytes.
    inner = r'stty raw -echo; printf "\033[>1u\033[?1000h\033[?1006h"; cat -v'
    pid, fd = spawn(inner=["--", "/bin/sh", "-c", inner])
    drain(fd)

    os.write(fd, b"\x02")  # legacy ctrl-b in
    out = drain(fd, idle=0.3, limit=2.0).decode("utf-8", "replace")
    check("ctrl-b -> kitty CSI u for a kitty pane", "^[[98;5u" in out, repr(out[-80:]))

    os.write(fd, b"\x1b[<0;7;3M")  # SGR mouse press in
    out = drain(fd, idle=0.3, limit=2.0).decode("utf-8", "replace")
    check("mouse reaches a pane that asked for it", "^[[<0;7;3M" in out,
          repr(out[-80:]))

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
