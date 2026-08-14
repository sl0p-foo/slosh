#!/usr/bin/env python3
"""A pane whose program exits stays until it is dismissed.

The old behaviour reaped it before the next paint, which meant the last thing
a failing command printed vanished along with the exit status that would have
explained it. Now the pane keeps its contents, says why it is over in its own
backlog and in its frame, and offers the only two verbs left that mean
anything: run it again, or close it.

Buttons are found through the hit list rather than by column, because where
they land is the frame's business and not this test's.
"""
import sys
import tempfile

from harness import Session, check, report

# Exits with a status, having printed something worth keeping.
DIES = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; echo mark-one; exit 3']
# Alive until told otherwise: it waits for a line, then exits. A `cat` in raw
# mode would not do — raw mode is exactly where EOT stops meaning end-of-file.
LIVES = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; read x']
# The same, having painted a known non-default colour into its own content.
GREEN = ["/bin/sh", "-c",
         'printf "\\033]2;p\\007"; printf "\\033[38;2;0;255;0mBLOCK\\033[0m\\n"; '
         'read x']
DONE = "q\\n"  # the line those two are waiting for


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def hit(snap, action):
    """(x, y) of the first cell registering an action, or None."""
    for e in snap.hits:
        if e["action"] == action:
            return e["x"], e["y"]
    return None


def test_it_stays_and_says_why():
    with Session(DIES, cols=60, rows=12) as s:
        snap = s.until_text("[process exited")
        pane = s.pane()

        check("the pane is still there", len(s.panes()) == 1, str(s.panes()))
        check("and the session with it", s.alive())
        check("what it printed is still on screen", "mark-one" in snap.screen())
        check("the backlog says it exited",
              "[process exited: status 3]" in snap.screen(), snap.screen())
        check("the frame says so too", "exited: status 3" in snap.screen())

        check("json calls it dead", pane["alive"] is False, str(pane))
        check("with the status", pane["exit_code"] == 3, str(pane))
        check("and no signal", pane["exit_signal"] == 0, str(pane))


def test_a_clean_exit_is_not_dressed_up_as_a_failure():
    with Session(["/bin/sh", "-c", "true"], cols=60, rows=12) as s:
        snap = s.until_text("[process exited")
        check("no status number for 0",
              "[process exited]" in snap.screen(), snap.screen())
        check("and the code is reported as 0", s.pane()["exit_code"] == 0)


def test_a_signal_is_named_as_one():
    with Session(["/bin/sh", "-c", "kill -9 $$"], cols=60, rows=12) as s:
        snap = s.until_text("[process exited")
        check("the backlog says signal",
              "[process exited: signal 9]" in snap.screen(), snap.screen())
        check("and so does json", s.pane()["exit_signal"] == 9, str(s.pane()))


def test_the_frame_offers_the_two_verbs_that_are_left():
    with Session(DIES, cols=60, rows=12) as s:
        snap = s.until_text("[process exited")
        pane = s.pane()

        check("a re-run button", "[re-run]" in snap.screen(), snap.screen())
        check("a close button", "[close]" in snap.screen(), snap.screen())

        rerun = hit(snap, f"rerun:{pane['id']}")
        close = hit(snap, f"close:{pane['id']}")
        check("re-run is clickable", rerun is not None, str(snap.hits))
        check("close is clickable", close is not None, str(snap.hits))
        # Rightmost is the one that must survive a narrow frame.
        check("close sits to the right of re-run", close[0] > rerun[0],
              f"{rerun} {close}")

        check("the button is drawn where its hit is",
              "[re-run]" in snap.line(rerun[1]), snap.line(rerun[1]))


def test_a_live_pane_keeps_its_own_buttons():
    """The dead row replaces the pane's OSC buttons; it must not pre-empt them."""
    argv = ["/bin/sh", "-c",
            'printf "\\033]5577;1;buttons;go:Go\\033\\\\"; stty raw -echo; cat']
    with Session(argv, cols=60, rows=12) as s:
        snap = s.until_text("[Go]")
        check("a live pane's own button is drawn", "[Go]" in snap.screen())
        check("and no dead buttons are", "[re-run]" not in snap.screen(),
              snap.screen())


def test_rerun_runs_it_again_on_top_of_what_it_left():
    # Tall enough that both runs are still on screen rather than in the
    # scrollback: what is being asserted is that the first one was kept.
    with Session(DIES, cols=60, rows=20) as s:
        snap = s.until_text("[process exited")
        pane = s.pane()
        x, y = hit(snap, f"rerun:{pane['id']}")
        s.click(x, y)

        snap = s.until(lambda sn: sn.screen().count("mark-one") == 2)
        check("the command ran again",
              snap.screen().count("mark-one") == 2, snap.screen())
        check("the run that ended is still above it",
              "[process exited: status 3]" in snap.screen(), snap.screen())
        check("the pane kept its id", s.pane()["id"] == pane["id"])

        snap = s.until(lambda sn: sn.screen().count("[process exited") == 2)
        check("and dying again says so again",
              snap.screen().count("[process exited: status 3]") == 2,
              snap.screen())
        check("with the buttons back", "[re-run]" in snap.screen())


def test_rerun_over_the_control_api():
    with Session(DIES, cols=60, rows=20) as s:
        s.until_text("[process exited")
        pane = s.pane()
        reply = s.api("rerun", id=pane["id"])
        check("the api runs it again", reply.get("ok") is True, str(reply))
        snap = s.until(lambda sn: sn.screen().count("mark-one") == 2)
        check("and it really ran", snap.screen().count("mark-one") == 2,
              snap.screen())

        reply = s.api("rerun", id=9999)
        check("an unknown pane is refused", reply.get("ok") is False, str(reply))


def test_the_keyboard_can_do_it_too():
    with Session(DIES, cols=60, rows=20) as s:
        s.until_text("[process exited")
        s.key("r")
        snap = s.until(lambda sn: sn.screen().count("mark-one") == 2)
        check("C-a r runs it again", snap.screen().count("mark-one") == 2,
              snap.screen())


def test_a_live_pane_is_not_restarted_under_you():
    """"Run it again" on something still running would mean killing it."""
    with Session(LIVES, cols=60, rows=12) as s:
        s.until_text("p")
        s.key("r")
        s.settle()
        snap = s.snapshot()
        check("it says no rather than doing it", "still running" in snap.screen(),
              snap.screen())
        check("and the pane is untouched", s.pane()["alive"] is True)


def test_closing_it_is_what_takes_it_away():
    with Session(LIVES, cols=80, rows=16) as s:
        s.until_text("p")
        s.key("\\\\")          # split, so closing one leaves a session behind
        s.settle()
        panes = s.panes()
        check("two panes", len(panes) == 2, str(panes))

        # End the focused one's program and let it die.
        s.raw(DONE)
        snap = s.until(lambda sn: "[process exited" in sn.screen())
        check("still two panes", len(s.panes()) == 2, str(s.panes()))

        dead = [p for p in s.panes() if not p["alive"]][0]
        x, y = hit(snap, f"close:{dead['id']}")
        s.click(x, y)
        s.settle()
        check("closing leaves one", len(s.panes()) == 1, str(s.panes()))
        check("and it is the live one", s.panes()[0]["alive"] is True)


def test_keep_dead_false_is_the_old_behaviour():
    with Session(LIVES, cols=80, rows=16,
                 config=cfg("keep_dead false\n")) as s:
        s.until_text("p")
        s.key("\\\\")
        s.settle()
        check("two panes", len(s.panes()) == 2, str(s.panes()))
        s.raw(DONE)
        snap = s.until(lambda sn: len(s.panes()) == 1)
        check("the dead pane is gone", len(s.panes()) == 1, str(s.panes()))
        check("and left no notice", "[process exited" not in snap.screen(),
              snap.screen())


def test_dying_recolours_what_the_pane_left_behind():
    """`dead` is a shader state now, which it could not be while panes were
    reaped before the next paint. Asserted as a change, not as a colour: which
    grey is the config's business."""
    with Session(GREEN, cols=60, rows=12) as s:
        snap = s.until_text("BLOCK")
        x, y = snap.find("BLOCK")
        live = snap.style_at(x, y)
        check("a live pane keeps the colour it printed",
              live and live.get("fg") == "#00ff00", str(live))

        s.raw(DONE)
        snap = s.until(lambda sn: "[process exited" in sn.screen())
        x, y = snap.find("BLOCK")
        dead = snap.style_at(x, y)
        check("a dead one does not",
              dead and dead.get("fg") != "#00ff00", str(dead))


def test_a_flattened_tab_still_says_which_pane_is_over():
    """A collapsed header is all a flattened tab draws of a pane, and the
    shader pass does not reach one — so the words have to carry it."""
    with Session(LIVES, cols=100, rows=20) as s:
        s.until_text("p")
        s.key("\\\\")
        s.settle()
        s.raw(DONE)                      # the focused (right-hand) one dies
        s.until(lambda sn: "[process exited" in sn.screen())
        s.key("h")                       # focus the live one, so the dead one
        s.settle()                       # is the pane that collapses
        s.resize(46, 14)                 # no room for two: the tab flattens
        s.settle()

        snap = s.snapshot()
        header = [l for l in snap.text if "· exited" in l]
        check("the header says it exited", header, snap.screen())


def test_the_status_line_names_the_state():
    with Session(DIES, cols=60, rows=12) as s:
        snap = s.until_text("[process exited")
        check("the focused dead pane is called exited",
              "exited: status 3" in snap.line(snap.rows - 2),
              snap.line(snap.rows - 2))


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
