"""Drive sl0ppty's headless driver and assert on the composited screen.

    with Session(["/bin/sh", "-c", "echo hi"], cols=20, rows=3) as s:
        s.settle()
        assert s.snapshot().line(0).strip() == "hi"

No pty, no sleeps beyond settling, no screen scraping: the compositor tells us
what it composed. This is the harness DESIGN.md asks for, and everything from
M1 on is tested through it.
"""
import json
import os
import subprocess
import time

# $SL0PPTY_BIN lets the same suite drive a differently-built binary — the
# coverage build, say — without a second copy of the harness.
BIN = os.environ.get(
    "SL0PPTY_BIN",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "sl0ppty"))


class Snapshot:
    def __init__(self, data):
        self.data = data
        self.text = data["text"]
        self.cursor = data["cursor"]
        self.styles = data["styles"]
        self.hits = data["hits"]
        self.cols = data["cols"]
        self.rows = data["rows"]

    def line(self, y):
        return self.text[y]

    def screen(self):
        return "\n".join(self.text)

    def find(self, needle):
        """(x, y) of the first occurrence, or None."""
        for y, row in enumerate(self.text):
            x = row.find(needle)
            if x >= 0:
                return x, y
        return None

    def style_at(self, x, y):
        """The style run covering a cell, or None if the cell is unstyled."""
        for run in self.styles:
            if run["y"] == y and run["x"] <= x < run["x"] + run["w"]:
                return run
        return None

    def pane_line(self, pane, row):
        """One row of a pane's content area, chrome excluded."""
        y = pane["content_y"] + row
        x = pane["content_x"]
        return self.text[y][x:x + pane["content_w"]]

    def pane_text(self, pane):
        return "\n".join(
            self.pane_line(pane, r) for r in range(pane["content_h"]))

    def hit_at(self, x, y):
        """The action registered at a cell, last painted winning."""
        for e in reversed(self.hits):
            if e["x"] <= x < e["x"] + e["w"] and e["y"] <= y < e["y"] + e["h"]:
                return e["action"]
        return None

    def __str__(self):
        return self.screen()


class Session:
    def __init__(self, argv, cols=80, rows=24, config=None, env=None,
                 layout=None):
        self.cols, self.rows = cols, rows
        environ = dict(os.environ)
        if config:
            environ["SL0PPTY_CONFIG"] = str(config)
        else:
            environ["SL0PPTY_CONFIG"] = "/nonexistent/sl0ppty.kdl"
        if env:
            environ.update(env)
        cmd = [BIN, "--script", "--cols", str(cols), "--rows", str(rows)]
        if layout:
            cmd += ["--layout", str(layout)]
        self.proc = subprocess.Popen(
            cmd + ["--"] + argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
            env=environ,
        )

    # -- commands ---------------------------------------------------------
    def _cmd(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def send(self, escaped):
        """Bytes as if typed at the outer terminal (decoded, then re-encoded)."""
        self._cmd(f"send {escaped}")

    def key(self, chord):
        r"""A prefixed command, e.g. key('\\\\') to split. C-a is \x01."""
        self._cmd(f"send \\x01{chord}")

    def click(self, x, y, button=0):
        self._cmd(f"send \\e[<{button};{x + 1};{y + 1}M")
        self._cmd(f"send \\e[<{button};{x + 1};{y + 1}m")

    def raw(self, escaped):
        """Bytes straight into the pane's pty, decoder bypassed."""
        self._cmd(f"raw {escaped}")

    def settle(self, ms=20):
        """Pump every pane until none has produced output for `ms`.

        The default is deliberately short. `settle` is a *duration*, and a
        duration is a guess: too long and every call donates dead time to every
        run forever, too short and it races. Where the answer is observable,
        use until()/until_text() instead and stop guessing.
        """
        self._cmd(f"settle {ms}")

    def until(self, pred, timeout_ms=3000, step_ms=10):
        """Pump until pred(snapshot) holds, then return that snapshot.

        `settle N` is the only thing that reads a pane's pty, so waiting for a
        program's output means pumping — but it is also a *duration*, and a
        duration is a guess about something observable. This polls the thing
        itself and returns the instant it is true, which is both faster than a
        generous settle and safer than a tight one: the deadline only bounds
        failure, never success.

        Returns the last snapshot either way, so a caller's own check reports
        what was actually on screen rather than a timeout from in here.
        """
        deadline = time.monotonic() + timeout_ms / 1000.0
        while True:
            self.settle(step_ms)
            snap = self.snapshot()
            if pred(snap) or time.monotonic() >= deadline:
                return snap

    def until_text(self, needle, **kw):
        """The common case: wait for a program to have printed something."""
        return self.until(lambda snap: needle in snap.screen(), **kw)

    def resize(self, cols, rows):
        self.cols, self.rows = cols, rows
        self._cmd(f"resize {cols} {rows}")

    def snapshot(self):
        self._cmd("snapshot json")
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("sl0ppty exited before answering snapshot")
        return Snapshot(json.loads(line))

    def api(self, cmd, **kw):
        """One JSON control request; returns the parsed reply."""
        kw["cmd"] = cmd
        self._cmd(json.dumps(kw))
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("sl0ppty exited before answering " + cmd)
        return json.loads(line)

    def tabs(self):
        self._cmd("tabs")
        return json.loads(self.proc.stdout.readline())

    def panes(self):
        self._cmd("panes")
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("sl0ppty exited before answering panes")
        return json.loads(line)

    def pane(self, n=0):
        return self.panes()[n]

    def focused(self):
        for p in self.panes():
            if p["focused"]:
                return p
        return None

    def alive(self):
        self._cmd("alive")
        return self.proc.stdout.readline().strip() == "true"

    def close(self):
        try:
            self._cmd("quit")
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


# -- tiny test runner ------------------------------------------------------

_fails = []


def check(name, cond, detail=""):
    if cond:
        print(f"ok   {name}")
    else:
        _fails.append(name)
        print(f"FAIL {name}  <- {detail}")


def report():
    print(f"\n{'FAILED' if _fails else 'all green'} ({len(_fails)} failures)")
    return 1 if _fails else 0
