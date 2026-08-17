"""How much history a pane keeps, and the two settings that decide it.

lib-vt is handed no limit and picks its own: 10,000 *bytes*, which measured at 622
lines of an 80-column pane. Nobody chose that number, and nothing could change it
— D4 promised scrollback and then left the size to a library default.
"""
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Session, check, report

TOP = r"\x01\x1b[1~"  # C-a Home: scroll to the oldest line


def spew(n):
    """A pane that prints `n` numbered lines and then waits."""
    return ["/bin/sh", "-c",
            'i=1; while [ $i -le %d ]; do echo "line-$i"; i=$((i+1)); done; read x'
            % n]


def conf(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def oldest(s):
    """The oldest line still in the pane, by scrolling to the top of it.

    Measured from what is on screen rather than from a counter, because the
    number a caller cares about is how far back they can *read*."""
    s.settle(400)
    s.send(TOP)
    screen = s.snapshot().screen()
    got = re.findall(r"line-(\d+)", screen)
    return min(int(g) for g in got) if got else None


def kept(s, printed):
    o = oldest(s)
    return None if o is None else printed - o + 1


def test_a_pane_keeps_the_configured_number_of_lines():
    """Not the exact number: lib-vt prunes a page at a time, so what a pane keeps
    is within a page's worth of what it was told. The property that matters is that
    the setting is what decides it."""
    cfg = conf("scrollback 400\n")
    with Session(spew(3000), cols=80, rows=8, config=cfg) as s:
        n = kept(s, 3000)
        check("a small setting is honoured", n is not None and 250 < n < 700,
              "kept %s of 3000" % n)
    os.unlink(cfg)

    cfg = conf("scrollback 2000\n")
    with Session(spew(3000), cols=80, rows=8, config=cfg) as s:
        n = kept(s, 3000)
        check("and a larger one keeps more", n is not None and n > 1500,
              "kept %s of 3000" % n)
    os.unlink(cfg)


def test_zero_keeps_no_history_at_all():
    """The screen, and nothing behind it."""
    cfg = conf("scrollback 0\n")
    with Session(spew(3000), cols=80, rows=8, config=cfg) as s:
        n = kept(s, 3000)
        check("nothing is retained past the viewport", n is not None and n <= 10,
              "kept %s of 3000" % n)
        check("and scrolling up says so", "scrolled" not in s.snapshot().screen(),
              s.snapshot().screen())
    os.unlink(cfg)


def test_the_default_is_not_a_library_accident():
    """622 lines was what lib-vt's 10,000-byte default bought. A `make` run is
    longer than that, which is exactly when scrollback is wanted."""
    with Session(spew(3000), cols=80, rows=8) as s:
        n = kept(s, 3000)
        check("the default keeps everything a 3000-line run printed",
              n is not None and n > 2900, "kept %s of 3000" % n)


def test_the_byte_ceiling_is_the_other_limit():
    """Either can be reached first and neither can see what the other depends on:
    a line count cannot know how wide the terminal is, a byte count cannot know
    how many lines that bought. lib-vt applies whichever bites."""
    cfg = conf("scrollback 5000\nscrollback_bytes 500000\n")
    with Session(spew(3000), cols=80, rows=8, config=cfg) as s:
        n = kept(s, 3000)
        check("a low ceiling clips a high line count",
              n is not None and n < 1200, "kept %s of 3000" % n)
    os.unlink(cfg)

    cfg = conf("scrollback 5000\nscrollback_bytes 0\n")
    with Session(spew(3000), cols=80, rows=8, config=cfg) as s:
        n = kept(s, 3000)
        check("and no ceiling lets the line count stand",
              n is not None and n > 2900, "kept %s of 3000" % n)
    os.unlink(cfg)


def test_a_negative_is_refused_rather_than_wrapped():
    """A signed value read into a size_t is how "unlimited" gets into a program by
    accident: eighteen quintillion lines, and nobody asked for it."""
    from harness import BIN
    import subprocess
    for line, word in (("scrollback -1\n", "scrollback is a number of lines"),
                       ("scrollback_bytes -1\n", "scrollback_bytes is a byte")):
        cfg = conf(line)
        out = subprocess.run([BIN, "--check", cfg], capture_output=True, text=True)
        said = out.stderr + out.stdout
        check("it says what it wanted: " + word, word in said, said)
        check("and exits non-zero", out.returncode == 1, str(out.returncode))
        os.unlink(cfg)


def test_it_applies_to_the_next_pane_not_the_ones_holding_history():
    """Same rule as `shell`, and for the same reason: retroactively shrinking a
    pane's scrollback would throw away output somebody is reading."""
    cfg = conf("scrollback 2000\n")
    with Session(spew(3000), cols=80, rows=8, config=cfg) as s:
        before = kept(s, 3000)
        check("the pane has its history", before is not None and before > 1500,
              "kept %s" % before)

        with open(cfg, "w") as f:
            f.write("scrollback 0\n")
        r = s.api("reload")
        check("the config reloads", r["ok"], str(r))
        after = kept(s, 3000)
        check("and the pane already open keeps what it had",
              after is not None and after > 1500, "kept %s" % after)
    os.unlink(cfg)


if __name__ == "__main__":
    for name, fn in sorted(list(globals().items())):
        if name.startswith("test_"):
            fn()
    sys.exit(report())
