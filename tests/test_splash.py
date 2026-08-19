#!/usr/bin/env python3
"""The logo splash: a greeting on attach, briefly, wearing a shader effect.

A scripted session never attaches, so these drive it through the control
API's `splash` verb -- the same call the server makes on MSG_HELLO. What is
asserted is the contract: centered, temporary, ended early by input, and
absent when splash_ms says never.
"""

import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']
GLYPHS = "\u2584\u2588\u2588\u2588\u2588\u2588\u2588\u2584"  # ▄██████▄, line one


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def test_the_splash_shows_centered_and_goes_away():
    conf = cfg("splash_ms 400\n")
    with Session(SH, cols=80, rows=24, config=conf) as s:
        s.settle(30)
        check("no greeting until asked", GLYPHS not in s.snapshot().screen(), "")
        s.api("splash")
        snap = s.snapshot()
        check("the logo is up", GLYPHS in snap.screen(), snap.screen()[:200])

        # Centered: the logo is 44 columns in an 80-column screen, so its
        # first glyph lands at (80 - 44) / 2 give or take the box margin.
        rows = [i for i, line in enumerate(snap.text) if GLYPHS in line]
        line = snap.text[rows[0]]
        x = line.index("\u2584")
        check("h-centered", 14 <= x <= 22, f"x={x}")
        check("v-centered", 6 <= rows[0] <= 12, f"y={rows[0]}")

        snap = s.until(lambda sn: GLYPHS not in sn.screen())
        check("and it goes away by itself", GLYPHS not in snap.screen(), "")
    os.unlink(conf)


def test_any_key_ends_it_and_still_lands():
    conf = cfg("splash_ms 10000\n")  # long enough that expiry cannot pass this
    with Session(SH, cols=80, rows=24, config=conf) as s:
        s.settle(30)
        s.api("splash")
        check("up", GLYPHS in s.snapshot().screen(), "")
        s.send("marker")  # plain keys: dismiss the greeting, reach the shell
        snap = s.until(lambda sn: GLYPHS not in sn.screen())
        check("a keystroke puts it away", GLYPHS not in snap.screen(), "")
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
        check("nothing appears", GLYPHS not in s.snapshot().screen(), "")
    os.unlink(conf)


def test_a_screen_too_small_gets_no_greeting():
    """Cropped branding is worse than none: the box needs its 48 columns."""
    conf = cfg("splash_ms 10000\n")
    with Session(SH, cols=40, rows=12, config=conf) as s:
        s.settle(30)
        s.api("splash")
        s.settle(30)
        check("no logo on 40 columns", GLYPHS not in s.snapshot().screen(), "")
    os.unlink(conf)


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
