"""Drive sl0ptty's headless driver and assert on the composited screen.

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

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "sl0ptty")


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
    def __init__(self, argv, cols=80, rows=24):
        self.cols, self.rows = cols, rows
        self.proc = subprocess.Popen(
            [BIN, "--script", "--cols", str(cols), "--rows", str(rows), "--"] + argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
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

    def settle(self, ms=120):
        self._cmd(f"settle {ms}")

    def resize(self, cols, rows):
        self.cols, self.rows = cols, rows
        self._cmd(f"resize {cols} {rows}")

    def snapshot(self):
        self._cmd("snapshot json")
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("sl0ptty exited before answering snapshot")
        return Snapshot(json.loads(line))

    def panes(self):
        self._cmd("panes")
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("sl0ptty exited before answering panes")
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
