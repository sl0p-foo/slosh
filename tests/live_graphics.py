#!/usr/bin/env python3
"""Big kitty images over the real socket, with a reader that stalls.

The headless suite proves the graphics *model*; this proves the delivery path
it cannot see: a pane image is re-emitted to the client as decoded RGBA, which
is megabytes of base64 in one indivisible message. Two bugs lived there, both
invisible headless:

  - the outbox cap was applied to backlog *plus message*, so a message bigger
    than the cap was silently undeliverable forever -- and the model advanced
    anyway, losing the deletions that rode in the dropped frame. The visible
    symptom was a screenshot parked at a fixed spot on the screen, immune to
    scrolling, forever.
  - once the cap bounded only the backlog, a queued image legitimately held
    the backlog over the cap until it drained -- and push_frame read that as
    a dead client and closed the connection. The visible symptom was the
    session auto-detaching whenever an image crossed the wire.

So: a pane draws an image whose transmission exceeds MAX_OUTBOX, while the
client reads nothing. The session must stay attached, the whole transmission
must arrive once the reader comes back, and scrolling the image away must
deliver its deletion.
"""

import fcntl
import os
import pty
import re
import select
import struct
import sys
import termios
import time
import uuid

BIN = os.environ.get(
    "SLOSH_BIN", os.path.join(os.path.dirname(__file__), "..", "build", "slosh")
)
SESSION = "test-" + uuid.uuid4().hex[:8]

# 1100x1000 RGB is 3.3MB raw, 4.4MB as base64: bigger than the server's 4MB
# outbox cap, so delivery *requires* the cap to bound backlog, not messages.
CHILD = r"""
import base64, sys, time
w, h = 1100, 1000
img = base64.b64encode(bytes([200, 60, 60]) * (w * h)).decode()
sys.stdout.write("\x1b_Ga=T,f=24,q=2,s=%d,v=%d,i=7,c=40,r=10,m=1;\x1b\\" % (w, h))
for off in range(0, len(img), 4096):
    last = off + 4096 >= len(img)
    sys.stdout.write("\x1b_Gm=%d;%s\x1b\\" % (0 if last else 1, img[off:off+4096]))
sys.stdout.write("\r\nIMG-SENT\r\n")
sys.stdout.flush()
time.sleep(2)  # visible for a few frames, like a screenshot you read past
for i in range(200):
    sys.stdout.write("line %d\r\n" % i)  # ...and then scrolled away
sys.stdout.flush()
time.sleep(30)
"""


def spawn(cols=80, rows=24):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-ghostty"
        os.environ["SLOSH_CONFIG"] = "/nonexistent/slosh.kdl"
        os.execv(BIN, [BIN, "-s", SESSION, "--", "python3", "-c", CHILD])
        os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    return pid, fd


def drain(fd, idle=0.4, limit=8.0):
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


def gfx_commands(buf):
    """(controls, payload_len) for every _G escape in the stream."""
    out = []
    for m in re.finditer(rb"\x1b_G([^\x1b]*)\x1b", buf):
        ctl, _, payload = m.group(1).partition(b";")
        out.append((ctl, len(payload)))
    return out


def main():
    ok = True
    pid, fd = spawn()

    # Say nothing, hear nothing: the child pours the image into its pty while
    # the client's own output backs up unread, which is what a slow terminal
    # over ssh looks like from the server's side.
    time.sleep(4.0)
    buf = drain(fd, idle=0.6, limit=30.0)

    ok &= check(
        "the display stayed attached through the flood",
        os.waitpid(pid, os.WNOHANG) == (0, 0),
        repr(buf[-120:]),
    )

    sent = 0
    for ctl, n in gfx_commands(buf):
        if b"a=t" in ctl or (b"a=" not in ctl and b"m=" in ctl):
            sent += n
    ok &= check(
        "a transmission bigger than the outbox cap arrives whole",
        sent >= (1100 * 1000 * 3 + 2) // 3 * 4,
        f"{sent} bytes of base64 reached the client",
    )
    cmds = gfx_commands(buf)
    placed = any(ctl.startswith(b"a=p") for ctl, _ in cmds)
    ok &= check("and the image is placed while visible", placed, repr(buf[-300:]))

    # The child's flood scrolled it away while nothing was being read: the
    # deletion rode a frame the full outbox may well have dropped, and it must
    # be said again until it lands -- or the client draws the placement at a
    # fixed spot forever, which is exactly how this was reported.
    deleted = any(ctl.startswith(b"a=d,d=i") for ctl, _ in cmds)
    ok &= check("scrolled away, the placement is deleted", deleted, repr(buf[-300:]))

    # Scrolling back up must re-place it, and from the transmission the client
    # already has: no second copy of the pixels.
    buf = b""
    for _ in range(20):
        os.write(fd, b"\x01\x1b[5~")  # C-a PageUp
        buf += drain(fd, idle=0.1, limit=3.0)
    buf += drain(fd, idle=0.4, limit=8.0)
    back = [ctl for ctl, _ in gfx_commands(buf)]
    ok &= check(
        "scrolled back, it is placed again",
        any(c.startswith(b"a=p") for c in back),
        repr(buf[-300:]),
    )
    ok &= check(
        "without retransmitting the pixels",
        not any(c.startswith(b"a=t") for c in back),
        str(back[:6]),
    )

    # A second terminal has its own kitty image namespace. The first client's
    # `sent` bit must not make a new viewer receive only a placement for pixels
    # its terminal has never seen.
    pid2, fd2 = spawn()
    second = drain(fd2, idle=0.6, limit=30.0)
    second_cmds = gfx_commands(second)
    ok &= check(
        "a joining client receives its own image transmission",
        any(c.startswith(b"a=t") for c, _ in second_cmds),
        str(second_cmds[:8]),
    )
    ok &= check(
        "and joining it leaves the first client attached",
        os.waitpid(pid, os.WNOHANG) == (0, 0),
    )

    os.write(fd2, b"\x01q")
    drain(fd2, idle=0.2, limit=2.0)
    for child in (pid, pid2):
        try:
            os.kill(child, 9)
        except ProcessLookupError:
            pass
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
