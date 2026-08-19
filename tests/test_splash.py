#!/usr/bin/env python3
"""The logo splash: a greeting on attach, briefly, wearing a shader effect.

A scripted session never attaches, so these drive it through the control
API's `splash` verb -- the same call the server makes on MSG_HELLO. What is
asserted is the contract: the glyphs fly in and land exactly, centered,
temporary, colourful, ended early by input, and absent when splash_ms says
never. Assembly takes the first two fifths of splash_ms, which is why each
test picks a duration to suit what it is watching.
"""

import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']
GLYPHS = "\u2584\u2588\u2588\u2588\u2588\u2588\u2588\u2584"  # ▄██████▄, line one
BLOCKS = ("\u2584", "\u2588", "\u2580")  # any glyph a particle could be


def flying(snap):
    return any(b in snap.screen() for b in BLOCKS)


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def test_the_splash_assembles_centered_and_goes_away():
    conf = cfg("splash_ms 1600\n")  # assembly done by 640ms, margin under load
    with Session(SH, cols=80, rows=24, config=conf) as s:
        s.settle(30)
        check("no greeting until asked", not flying(s.snapshot()), "")
        s.api("splash")

        # The logo's first line contains its opening run three times over, and
        # assembly is staggered, so a middle occurrence can land before the
        # leftmost: "assembled" is the row whose *first* block glyph is where
        # the match begins.
        def landed_row(sn):
            for i, line in enumerate(sn.text):
                if GLYPHS in line and line.index(GLYPHS) == line.index("\u2584"):
                    return i
            return None

        snap = s.until(lambda sn: landed_row(sn) is not None, timeout_ms=6000)
        y = landed_row(snap)
        check("the glyphs land as the logo", y is not None, snap.screen()[:200])

        # Centered: the logo is 44 columns in an 80-column screen, so its
        # first glyph lands at (80 - 44) / 2 give or take the box margin.
        x = snap.text[y].index(GLYPHS) if y is not None else -1
        check("h-centered", 14 <= x <= 22, f"x={x}")
        check("v-centered", y is not None and 6 <= y <= 12, f"y={y}")

        snap = s.until(lambda sn: not flying(sn))
        check("and it goes away by itself", not flying(snap), "")
    os.unlink(conf)


def test_every_motion_lands_exactly():
    """Five ways to fly in, one place to land: whatever the motion, assembly
    ends with every particle on its cell -- an almost-assembled logo reads as
    a bug, which is what the per-particle delay rescaling exists to prevent."""
    conf = cfg("splash_ms 1600\n")  # landed window: 960ms, wide enough under load
    with Session(SH, cols=80, rows=24, config=conf) as s:
        s.settle(30)
        for motion in range(5):
            s.api("splash", fx=0, motion=motion)
            # Landed means the line exists *and* nothing is still passing to
            # its left: the first block glyph on that row is the logo's own.
            landed = lambda sn: any(
                GLYPHS in line and line.index("\u2584") == 18 for line in sn.text
            )
            snap = s.until(landed, timeout_ms=6000)
            check(
                f"motion {motion} lands the logo, exactly placed",
                landed(snap),
                snap.screen()[:200],
            )
            s.until(lambda sn: not flying(sn), timeout_ms=6000)  # expire between rounds
    os.unlink(conf)


def test_the_splash_is_colourful_not_a_tinted_logo():
    """The first version painted every effect in the theme accent, which made
    six effects into one dull one. An effect is a chain of vivid tints now, so
    the logo must show genuinely different *hues* -- counted as dominant-
    channel families, because two hundred shades of the same blue is exactly
    the failure this guards against."""
    conf = cfg("splash_ms 2500\n")  # assembled by 1000ms, coloured until 2500
    with Session(SH, cols=80, rows=24, config=conf) as s:
        s.settle(30)
        s.api("splash", fx=0)  # spectrum: deterministic, and the loudest
        snap = s.until(lambda sn: GLYPHS in sn.screen(), timeout_ms=6000)
        rows = [i for i, line in enumerate(snap.text) if GLYPHS in line]
        families = set()
        for y in rows:
            for x in range(16, 64):
                st = snap.style_at(x, y)
                fg = (st or {}).get("fg")
                if not fg or fg == "#000000":
                    continue
                r, g, b = int(fg[1:3], 16), int(fg[3:5], 16), int(fg[5:7], 16)
                families.add((r > g + 32, g > b + 32, r > b + 32))
        check(
            "the logo wears several hues at once",
            len(families) >= 3,
            f"{len(families)} families: {sorted(families)}",
        )
    os.unlink(conf)


def test_any_key_ends_it_mid_flight_and_still_lands():
    conf = cfg("splash_ms 10000\n")  # long enough that expiry cannot pass this
    with Session(SH, cols=80, rows=24, config=conf) as s:
        s.settle(30)
        s.api("splash")
        snap = s.until(flying)
        check("particles are in the air", flying(snap), "")
        s.send("marker")  # plain keys: dismiss the greeting, reach the shell
        snap = s.until(lambda sn: not flying(sn))
        check("a keystroke puts the whole flight away", not flying(snap), "")
        snap = s.until_text("marker")
        check(
            "...and still reaches the pane it was aimed at",
            "marker" in snap.screen(),
            snap.screen()[:200],
        )
    os.unlink(conf)


def test_splash_ms_zero_means_never():
    conf = cfg("splash_ms 0\n")
    with Session(SH, cols=80, rows=24, config=conf) as s:
        s.settle(30)
        s.api("splash")
        s.settle(30)
        check("nothing appears", not flying(s.snapshot()), "")
    os.unlink(conf)


def test_a_screen_too_small_gets_no_greeting():
    """Cropped branding is worse than none: the box needs its 48 columns."""
    conf = cfg("splash_ms 10000\n")
    with Session(SH, cols=40, rows=12, config=conf) as s:
        s.settle(30)
        s.api("splash")
        s.settle(30)
        check("no logo on 40 columns", not flying(s.snapshot()), "")
    os.unlink(conf)


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
