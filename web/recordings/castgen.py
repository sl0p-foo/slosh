#!/usr/bin/env python3
"""Record a scripted slosh session into a sloshcast.

A recording here is a *program*: a demo script drives `slosh --script`
(through tests/harness.py, the same Session the test suite uses), advances a
virtual clock with authored pauses and a typing cadence, and captures the
compositor's own snapshot after every action. Frames arrive with colours
already resolved to hex by the theme, so the browser player needs no VT, no
palette, no wasm -- it replays styled rows into a DOM grid.

The format ("sloshcast", one JSON file):

    {
      "v": 1,
      "cols": 100, "rows": 28,          # the grid at t=0
      "fg": "#ffffff", "bg": "#000000", # the theme's defaults
      "title": "...", "dur": 14.2,      # seconds, including the tail
      "events": [
        {"t": 0.0,
         "rows": {"0": [text, [[x, w, fg, bg, attrbits], ...]], ...},
         "cur": [x, y]},                # only what changed; event 0 is full
        {"t": 3.1, "size": [80, 20], "rows": {...}},   # a resize: full frame
        {"t": 5.0, "ptr": [40, 12]},   # the pointer track: cell, [x,y,1]
        ...                            #   while the button is down, null gone
      ]
    }

attrbits: 1 bold, 2 italic, 4 underline, 8 dim, 16 reverse, 32 strike.
Timing is entirely virtual -- the same script yields the same file, byte for
byte, no matter how loaded the machine that rendered it was.
"""

import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from harness import Session  # noqa: E402

THEME = os.path.join(ROOT, "contrib", "themes", "sl0p.kdl")

ATTR_BITS = {
    "bold": 1,
    "italic": 2,
    "underline": 4,
    "dim": 8,
    "faint": 8,
    "reverse": 16,
    "inverse": 16,
    "strike": 32,
    "strikethrough": 32,
}


def _theme_defaults(path):
    """default_fg / default_bg out of a theme file, for the player's canvas."""
    fg, bg = "#ffffff", "#000000"
    try:
        text = open(path).read()
        m = re.search(r'default_fg\s+"(#[0-9a-fA-F]{6})"', text)
        if m:
            fg = m.group(1)
        m = re.search(r'default_bg\s+"(#[0-9a-fA-F]{6})"', text)
        if m:
            bg = m.group(1)
    except OSError:
        pass
    return fg, bg


class Recorder:
    def __init__(
        self,
        argv=None,
        cols=100,
        rows=28,
        title="",
        config=THEME,
        env=None,
        layout=None,
    ):
        self.s = Session(
            list(argv or ["/bin/sh"]),
            cols=cols,
            rows=rows,
            config=config,
            env=env,
            layout=layout,
        )
        self.api = self.s.api
        self.title = title
        self.fg, self.bg = _theme_defaults(config) if config else ("#ffffff", "#000000")
        self.t = 0.0
        self.events = []
        self._size = (cols, rows)
        self._size0 = (cols, rows)
        self._rows = None  # previous frame, list of [text, runs]
        self._cur = None
        self._seed = 0x5105  # deterministic typing jitter
        self._ptr = None  # pointer as last emitted: [x, y] / [x, y, 1] / None
        self._at = None  # pointer cell right now, (x, y)
        self._down = False

    # -- the clock and the capture ---------------------------------------

    def _jitter(self):
        self._seed = (self._seed * 1103515245 + 12345) & 0x7FFFFFFF
        return self._seed / 0x7FFFFFFF

    def _frame(self, snap):
        runs_by_y = {}
        for r in snap.styles:
            bits = 0
            for a in r.get("attrs", ()):
                bits |= ATTR_BITS.get(a, 0)
            runs_by_y.setdefault(r["y"], []).append(
                [r["x"], r["w"], r["fg"], r["bg"], bits]
            )
        return [
            [text, sorted(runs_by_y.get(y, []))] for y, text in enumerate(snap.text)
        ]

    def _record(self, frame, cur, size, dt=0.0, force=False):
        """Advance the clock by dt, then record whatever changed."""
        self.t += dt

        ev = {"t": round(self.t, 3)}
        if size != self._size:
            ev["size"] = list(size)
            self._rows = None  # a resize event carries the full frame
        changed = {
            str(y): row
            for y, row in enumerate(frame)
            if self._rows is None or y >= len(self._rows) or self._rows[y] != row
        }
        if changed:
            ev["rows"] = changed
        if cur != self._cur:
            ev["cur"] = cur
        ptr = None if self._at is None else list(self._at) + ([1] if self._down else [])
        if ptr != self._ptr:
            ev["ptr"] = ptr
            self._ptr = ptr

        if len(ev) > 1 or force:
            self.events.append(ev)
        self._rows, self._cur, self._size = frame, cur, size

    def capture(self, dt=0.0, settle_ms=20, force=False):
        """Advance the clock by dt, then record whatever changed on screen."""
        self.s.settle(settle_ms)
        snap = self.s.snapshot()
        cur = [snap.cursor["x"], snap.cursor["y"]] if snap.cursor["visible"] else None
        self._record(self._frame(snap), cur, (snap.cols, snap.rows), dt=dt, force=force)

    # -- more than one session in a recording -------------------------------

    def switch(self, session, dt=0.0, settle_ms=100):
        """Record from another live Session from here on. The next frame is
        full, which is what a real attach paints anyway. The session keeps
        living (and can be poked over its api) while it is off camera --
        that is the whole point of the detach demo."""
        self.s = session
        self.api = session.api
        self._rows = None
        self.capture(dt=dt, settle_ms=settle_ms, force=True)

    def shell(self, lines, dt=0.12):
        """A synthetic bare-terminal frame: the interstitial shell between
        detaching from one session and attaching the next. The writer owns
        the byte stream, so the frames between real segments can simply be
        authored -- plain rows, no styling, a cursor at the end."""
        cols, rows = self._size
        frame = [[l[:cols], []] for l in lines[:rows]]
        frame += [["", []] for _ in range(rows - len(frame))]
        cur = None
        if lines:
            y = min(len(lines), rows) - 1
            cur = [min(len(lines[-1]), cols - 1), y]
        self._record(frame, cur, self._size, dt=dt)

    def shell_type(self, lines, text, cps=0.07):
        """Type onto the last line of a synthetic shell, like a person."""
        base = lines[-1]
        for i in range(1, len(text) + 1):
            lines[-1] = base + text[:i]
            self.shell(lines, dt=cps * (0.6 + 0.8 * self._jitter()))

    # -- what a demo script says -----------------------------------------

    def pause(self, sec):
        """Authored silence. Also lands any output that arrived meanwhile.

        Time inside slosh is real: a toast expires and a shader animates on
        the monotonic clock, not ours. So when the session says it wants a
        next frame (deadline >= 0) within this pause, actually wait that long
        and sample the frame -- toasts fade mid-pause exactly as they would
        live, and an animation is captured at the cadence it asked for.
        Otherwise the pause costs nothing but the authored seconds."""
        left = sec
        while left > 0:
            self.capture()  # compose, so deadline() is about a real frame
            d = self.s.deadline()
            if d < 0 or d / 1000.0 >= left:
                break
            wait = max(d / 1000.0, 0.03)
            time.sleep(wait)  # let slosh's own clock reach the deadline
            left -= wait
            self.capture(dt=wait, settle_ms=5)
        self.capture(dt=left)

    def hold(self, sec):
        """Advance the clock without touching the session: authored silence
        over synthetic frames (the shell between two sessions), where pause()
        would ask a live session for its deadline."""
        self.t += sec

    def wall(self, sec, step=0.1):
        """Spend real seconds, sampling as they pass -- for a program in a
        pane that needs actual time (a background job about to ring the
        bell), where pause()'s deadline shortcut would skip past it."""
        n = max(1, int(sec / step))
        for _ in range(n):
            time.sleep(step)
            self.capture(dt=step, settle_ms=5)

    def type(self, text, cps=0.06, dt=0.0):
        """Type like a person: per-character frames, deterministic jitter."""
        self.capture(dt=dt)
        for ch in text:
            self.api("send", data=ch)
            self.capture(dt=cps * (0.6 + 0.8 * self._jitter()), settle_ms=8)

    def enter(self, dt=0.15):
        self.api("send", data="\r")
        self.capture(dt=dt)

    def run(self, text, wait=None, dt=0.35, **kw):
        """Type a command, press enter, optionally wait for its output."""
        self.type(text)
        self.enter()
        if wait is not None:
            self.s.until_text(wait, **kw)
        self.capture(dt=dt)

    def key(self, chord, dt=0.45):
        """A leader chord: key('\\\\') is C-a \\, key('c') is C-a c.

        The JSON `send` takes bytes verbatim (JSON strings carry control
        characters natively); the \\x01 escape language belongs to the
        bare-verb form only.
        """
        self.api("send", data="\x01" + chord)
        self.capture(dt=dt)

    def send(self, data, dt=0.3):
        """Bytes as if typed, verbatim -- pass real control characters."""
        self.api("send", data=data)
        self.capture(dt=dt)

    # -- the mouse ---------------------------------------------------------
    # The recording carries a pointer track ("ptr" events): the driver knows
    # the pointer's cell at every moment, so the player can draw the pointer
    # a screen capture never shows. Real SGR mouse bytes go to slosh at the
    # same time, so hover hints, drags and drops are the program's own.

    def _mouse(self, code, final="M"):
        x, y = self._at
        self.api("send", data="\x1b[<%d;%d;%d%s" % (code, x + 1, y + 1, final))

    def move_to(self, x, y, dur=0.5, hover=True):
        """Glide the pointer there in small steps; hover events go to slosh."""
        sx, sy = self._at if self._at else (x, y)
        steps = max(2, int(dur / 0.05))
        for i in range(1, steps + 1):
            self._at = (
                round(sx + (x - sx) * i / steps),
                round(sy + (y - sy) * i / steps),
            )
            if hover:
                self._mouse(35)
            self.capture(dt=dur / steps, settle_ms=5)

    def press(self, dt=0.15):
        self._down = True
        self._mouse(0, "M")
        self.capture(dt=dt)

    def release(self, dt=0.2):
        self._mouse(0, "m")
        self._down = False
        self.capture(dt=dt)

    def click(self, x, y, move=0.5, dt=0.5):
        self.move_to(x, y, dur=move)
        self.press()
        self.release(dt=dt)

    def drag_to(self, x, y, dur=0.7, dt=0.3):
        """With the button held: motion events slosh sees as a drag."""
        sx, sy = self._at
        steps = max(2, int(dur / 0.05))
        for i in range(1, steps + 1):
            self._at = (
                round(sx + (x - sx) * i / steps),
                round(sy + (y - sy) * i / steps),
            )
            self._mouse(32)
            self.capture(dt=dur / steps, settle_ms=5)
        self.capture(dt=dt)

    def resize(self, cols, rows, dt=0.4):
        self.s.resize(cols, rows)
        self.capture(dt=dt)

    def until_text(self, needle, dt=0.3, **kw):
        self.s.until_text(needle, **kw)
        self.capture(dt=dt)

    def snapshot(self):
        return self.s.snapshot()

    # -- the file ---------------------------------------------------------

    def save(self, path, tail=1.2):
        """Write the sloshcast; `tail` is how long the last frame holds."""
        if not self.events:
            self.capture(force=True)
        data = {
            "v": 1,
            "cols": self._size0[0],
            "rows": self._size0[1],
            "fg": self.fg,
            "bg": self.bg,
            "title": self.title,
            "dur": round(self.t + tail, 3),
            "events": self.events,
        }
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w") as f:
            json.dump(data, f, separators=(",", ":"))
        size = os.path.getsize(path)
        print(
            f"{path}: {len(self.events)} events, {data['dur']}s, {size / 1024:.1f} KiB"
        )


def out_path(default_name):
    """Where a demo writes: argv[1], or build/<name>.json beside this file."""
    if len(sys.argv) > 1:
        return sys.argv[1]
    return os.path.join(HERE, "build", default_name + ".json")
