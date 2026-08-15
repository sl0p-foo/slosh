#!/usr/bin/env python3
"""The config watcher, which needs a real server: inotify lives in its loop.

The interesting cases are not "does it notice a write" but the two that a naive
watch gets wrong -- an editor renaming a new file over the old one, and a file
caught mid-save that does not parse.
"""
import json
import os
import subprocess
import sys
import time
import uuid

BIN = os.environ.get(
    "SL0PPTY_BIN",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build",
                 "sl0ppty"))

fails = 0


def check(name, cond, detail=""):
    global fails
    if not cond:
        fails += 1
    print(f"{'ok  ' if cond else 'FAIL'} {name:56} {'' if cond else detail}")


def frame_colour(session):
    """The focused pane's frame colour, straight from the live session."""
    out = subprocess.run([BIN, "-s", session, "cmd", "snapshot"],
                         capture_output=True, text=True).stdout
    try:
        runs = json.loads(out)["styles"]
    except Exception:
        return None
    top = [r for r in runs if r["y"] == 2 and r.get("fg")]
    return top[0]["fg"] if top else None


def wait_for_colour(session, want, timeout=5.0):
    """Poll for the colour rather than sleeping a guess at how long it takes.
    Returns as soon as it lands, so the pass costs milliseconds."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        got = frame_colour(session)
        if got == want:
            return got
        time.sleep(0.02)
    return frame_colour(session)


def start(session, cfg_path, extra=()):
    env = dict(os.environ, SL0PPTY_CONFIG=cfg_path)
    p = subprocess.Popen([BIN, "--server", "-s", session, *extra,
                          "--cols", "60", "--rows", "12",
                          "--", "/bin/sh", "-c", "stty raw -echo; cat"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         env=env)
    for _ in range(200):                      # gate on the socket answering
        if frame_colour(session):
            return p
        time.sleep(0.02)
    return p


def stop(session, proc):
    subprocess.run([BIN, "-s", session, "cmd", '{"cmd":"quit"}'],
                   capture_output=True)
    try:
        proc.wait(timeout=3)
    except Exception:
        proc.kill()


def write(path, colour):
    with open(path, "w") as f:
        f.write('theme {\n  frame_focus "%s"\n}\n' % colour)


def test_watch():
    name = "t" + uuid.uuid4().hex[:8]
    cfg = f"/tmp/{name}.kdl"
    write(cfg, "#00ff00")
    p = start(name, cfg)
    try:
        check("the session starts with the config it was given",
              frame_colour(name) == "#00ff00", str(frame_colour(name)))

        write(cfg, "#ff0000")
        check("writing the file in place reloads it",
              wait_for_colour(name, "#ff0000") == "#ff0000",
              str(frame_colour(name)))

        # The case a watch on the *file* would miss: the inode is replaced, so
        # a file watch fires once and then never again.
        write(cfg + ".tmp", "#0000ff")
        os.rename(cfg + ".tmp", cfg)
        check("and so does an editor renaming a new file over it",
              wait_for_colour(name, "#0000ff") == "#0000ff",
              str(frame_colour(name)))

        with open(cfg, "w") as f:
            f.write('theme {\n  frame_focus "unterminated\n')
        time.sleep(0.4)   # bounded: proving nothing happened needs a wait
        check("a file that does not parse keeps what works",
              frame_colour(name) == "#0000ff", str(frame_colour(name)))
        alive = subprocess.run([BIN, "-s", name, "cmd", "alive"],
                               capture_output=True, text=True).stdout.strip()
        check("and the session is still alive to say so", alive == "true",
              repr(alive))

        write(cfg, "#ffff00")
        check("and it recovers once the file is valid again",
              wait_for_colour(name, "#ffff00") == "#ffff00",
              str(frame_colour(name)))
    finally:
        stop(name, p)
        for f in (cfg, cfg + ".tmp"):
            if os.path.exists(f):
                os.unlink(f)


def toast_count(session, text):
    """How many times `text` appears on screen. Toasts stack, so a config that
    reloaded twice announces itself twice."""
    out = subprocess.run([BIN, "-s", session, "cmd", "snapshot"],
                         capture_output=True, text=True).stdout
    try:
        rows = json.loads(out)["text"]
    except Exception:
        return -1
    return sum(1 for r in rows if text in r)


def test_one_save_is_one_reload():
    """An editor that creates the file rather than rewriting it produces two
    inotify events -- CREATE then CLOSE_WRITE -- and reloading on each would
    parse the config twice and say so twice for a single edit."""
    name = "t" + uuid.uuid4().hex[:8]
    cfg = f"/tmp/{name}.kdl"
    write(cfg, "#00ff00")
    p = start(name, cfg)
    try:
        # Recreate it the way an editor that unlinks first would.
        os.unlink(cfg)
        write(cfg, "#ff0000")
        check("the recreated file is picked up",
              wait_for_colour(name, "#ff0000") == "#ff0000",
              str(frame_colour(name)))
        time.sleep(0.3)   # bounded: let any second reload land if it is coming
        n = toast_count(name, "config reloaded")
        check("and announced exactly once, not once per inotify event",
              n == 1, f"{n} toasts")
    finally:
        stop(name, p)
        if os.path.exists(cfg):
            os.unlink(cfg)


def test_no_reload():
    name = "t" + uuid.uuid4().hex[:8]
    cfg = f"/tmp/{name}.kdl"
    write(cfg, "#00ff00")
    p = start(name, cfg, extra=("--no-reload",))
    try:
        check("--no-reload starts with the config all the same",
              frame_colour(name) == "#00ff00", str(frame_colour(name)))
        write(cfg, "#ff0000")
        time.sleep(0.5)   # bounded, for the same reason as above
        check("but does not pick up a change on its own",
              frame_colour(name) == "#00ff00", str(frame_colour(name)))
        # ...and asking explicitly still works, which is the point of the flag
        # being about *watching* rather than about reloading.
        subprocess.run([BIN, "-s", name, "cmd", '{"cmd":"reload"}'],
                       capture_output=True)
        check("while asking for a reload still works",
              wait_for_colour(name, "#ff0000") == "#ff0000",
              str(frame_colour(name)))
    finally:
        stop(name, p)
        if os.path.exists(cfg):
            os.unlink(cfg)


if __name__ == "__main__":
    test_watch()
    test_one_save_is_one_reload()
    test_no_reload()
    print()
    print(f"{'FAILED' if fails else 'all green'} ({fails} failures)")
    sys.exit(1 if fails else 0)
