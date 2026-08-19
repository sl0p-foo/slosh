#!/usr/bin/env python3
"""Chrome is laid out in columns, and some glyphs are two of them.

The bug: a bell mark like 🔔 is one character and two columns. Every length in
app.c came from counting characters, so the mark was booked as one column and
drawn as two, and everything after it on that row -- the title, the buttons,
the frame's right corner -- landed a column early.

Assertions here are on *columns*, taken from style runs rather than the text
dump: the dump collapses a wide cell into one character, so measuring a python
string is measuring the wrong thing and would have passed before the fix.
"""

import json
import os
import subprocess
import sys
import tempfile

from harness import BIN, check, report

LAYOUT = """layout {
  tab {
    pane focus=true
    pane command="printf '\\\\033]2;ringer\\\\007'; printf '\\\\a'; sleep 30"
  }
}
"""

NARROW = "\u2022"  # one column
WIDE = "\U0001f514"  # bell emoji, two columns
CLUSTER = "\U0001f468\u200d\U0001f469\u200d\U0001f467"  # ZWJ family: 18 bytes


def run(mark, cols=100, rows=14):
    lay = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    lay.write(LAYOUT)
    lay.close()
    cfg = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    # An unmistakable bell colour: the assertion is "how many columns did the
    # mark claim", which means finding those columns without guessing.
    # ...and without the default bell flash: it paints the whole frame in
    # that same colour at full strength, which would make "the first column
    # that is not the bell colour" the far corner of the pane.
    cfg.write('bell_mark "%s"\ntheme { bell "#00ff00" }\nstates { bell { } }\n' % mark)
    cfg.close()
    p = subprocess.run(
        [
            BIN,
            "--script",
            "--cols",
            str(cols),
            "--rows",
            str(rows),
            "--layout",
            lay.name,
            "--",
            "/bin/sh",
            "-c",
            "read x",
        ],
        input=b"settle 250\npanes\nsnapshot json\n",
        capture_output=True,
        env={**os.environ, "SLOSH_CONFIG": cfg.name},
    )
    os.unlink(lay.name)
    os.unlink(cfg.name)
    return p.stdout


def columns_claimed_by_the_mark(mark):
    """How many columns of the title row the mark actually owns.

    Measured from where it is drawn (one column in from the pane's left edge)
    to the first column that is not the bell colour. This is the number that
    was wrong: the glyph was drawn in two columns and booked as one, so the
    dashes, title and buttons after it all moved a column left."""
    lines = run(mark).decode().strip().split("\n")
    panes, snap = json.loads(lines[0]), json.loads(lines[1])
    rang = [q for q in panes if not q["focused"]][0]
    y, start = rang["y"], rang["x"] + 1
    runs = {r["x"]: r for r in snap["styles"] if r["y"] == y}
    n, x = 0, start
    while x in runs and runs[x].get("fg") == "#00ff00":
        n += runs[x]["w"]
        x += runs[x]["w"]
    return n


def test_a_mark_claims_exactly_the_columns_it_draws_in():
    for label, mark, want in (
        ("narrow", NARROW, 1),
        ("wide", WIDE, 2),
        ("two characters", "!!", 2),
    ):
        got = columns_claimed_by_the_mark(mark)
        check(
            "%s mark claims %d column(s)" % (label, want),
            got == want,
            "claimed %d" % got,
        )


def test_the_mark_is_actually_drawn():
    out = run(WIDE).decode()
    check("a wide mark reaches the screen", WIDE in out, "no bell mark on screen")


def test_no_mark_can_put_invalid_utf8_on_the_wire():
    """A mark is a grapheme cluster, not a character. The config stored it in a
    fixed buffer with snprintf, which truncates at a byte: an 18-byte family
    emoji came out as 15 bytes ending mid-codepoint, and every consumer of the
    snapshot then had a decoding error instead of a screen."""
    for label, mark in (("wide", WIDE), ("18-byte cluster", CLUSTER)):
        raw = run(mark)
        try:
            raw.decode("utf-8")
            ok = True
        except UnicodeDecodeError as e:
            ok = False
            detail = repr(raw[max(0, e.start - 20) : e.start + 12])
        check("%s: output is valid utf-8" % label, ok, detail if not ok else "")


def test_widths_come_from_the_terminals_own_table():
    """Not a table of our own: the same one lib-vt uses for pane content, so
    chrome and content cannot disagree about how wide something is."""
    src = open(os.path.join(os.path.dirname(__file__), "..", "src", "screen.c")).read()
    check(
        "width comes from lib-vt",
        "ghostty_unicode_grapheme_width" in src,
        "screen.c is measuring text some other way",
    )


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
