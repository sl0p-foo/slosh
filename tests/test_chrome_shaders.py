#!/usr/bin/env python3
"""`where="chrome"`: the same colour passes, run over a pane's frame.

A shader was always a pass over a pane's *contents*, and the frame was left
alone by construction — it is painted before the pass and lies outside the
content rect. Chrome shaders are the other half of that: the frame is a rect
too, so it is a pass too, and the same expressions that dim contents can make
a border say something. Nothing new in the language, one new word in the
config.

What is worth checking is the seam. The two passes must not reach into each
other's cells; the frame pass has to see the *whole frame's* coordinates or an
effect could not travel round one; and an animated pass has to keep the session
painting, or a pulse freezes the moment you stop typing.
"""
import os
import sys
import tempfile
import time

from harness import Session, check, report

# A pane that paints a known, non-default colour into its own content.
GREEN = "#00ff00"
FRAME = "#ff5fd7"    # the default focused frame colour
DIM_FRAME = "#45454a"  # ...and the colour of a frame you are not in
SH = ["/bin/sh", "-c",
      'printf "\\033]2;p\\007"; printf "\\033[38;2;0;255;0mBLOCK\\033[0m\\n"; '
      'stty raw -echo; cat']

THEME = 'theme { default_fg "#ffffff" default_bg "#000000" }\n'


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(THEME + text)
    f.close()
    return f.name


def fg_at(snap, x, y):
    return (snap.style_at(x, y) or {}).get("fg")


def frame_fg(snap, pane, dx=5, dy=0):
    return fg_at(snap, pane["x"] + dx, pane["y"] + dy)


def content_fg(snap, pane):
    return fg_at(snap, pane["content_x"], pane["content_y"])


def test_a_chrome_pass_colours_the_frame_and_nothing_else():
    conf = cfg('shaders {\n    tint color="#ff0000" amount=255 where="chrome"\n}\n')
    with Session(SH, cols=60, rows=12, config=conf) as s:
        s.until_text("BLOCK")
        p, snap = s.pane(), s.snapshot()
        check("the frame takes the chrome colour",
              frame_fg(snap, p) == "#ff0000", str(frame_fg(snap, p)))
        check("all four sides of it, not just the top",
              fg_at(snap, p["x"], p["y"] + 3) == "#ff0000" and
              fg_at(snap, p["x"] + p["w"] - 1, p["y"] + 3) == "#ff0000" and
              fg_at(snap, p["x"] + 5, p["y"] + p["h"] - 1) == "#ff0000",
              snap.screen())
        check("and the pane's own output is untouched",
              content_fg(snap, p) == GREEN, str(content_fg(snap, p)))


def test_a_content_pass_still_leaves_the_frame_alone():
    """The invariant the frame has always had, now that something else in the
    same block can legitimately reach it."""
    conf = cfg('shaders {\n    tint color="#ff0000" amount=255\n}\n')
    with Session(SH, cols=60, rows=12, config=conf) as s:
        s.until_text("BLOCK")
        p, snap = s.pane(), s.snapshot()
        check("the contents are tinted", content_fg(snap, p) == "#ff0000",
              str(content_fg(snap, p)))
        check("the frame is not", frame_fg(snap, p) == FRAME,
              str(frame_fg(snap, p)))


def test_the_two_passes_can_run_together():
    conf = cfg('shaders {\n'
               '    tint color="#0000ff" amount=255\n'
               '    tint color="#ff0000" amount=255 where="chrome"\n'
               '}\n')
    with Session(SH, cols=60, rows=12, config=conf) as s:
        s.until_text("BLOCK")
        p, snap = s.pane(), s.snapshot()
        check("each pass owns its own cells",
              content_fg(snap, p) == "#0000ff" and frame_fg(snap, p) == "#ff0000",
              f"{content_fg(snap, p)} / {frame_fg(snap, p)}")


def test_positions_are_the_whole_frames():
    """`y == 0` is the frame's top row, not the top row of one of its sides:
    an effect that travels round a border needs one coordinate space."""
    conf = cfg('shaders {\n'
               '    tint color="#00ff00" amount="(y == 0) * 255" where="chrome"\n'
               '}\n')
    with Session(SH, cols=60, rows=12, config=conf) as s:
        s.until_text("BLOCK")
        p, snap = s.pane(), s.snapshot()
        check("the top row is the row the expression selected",
              frame_fg(snap, p) == "#00ff00", str(frame_fg(snap, p)))
        check("the sides and the bottom are not",
              fg_at(snap, p["x"], p["y"] + 3) == FRAME and
              fg_at(snap, p["x"] + 5, p["y"] + p["h"] - 1) == FRAME,
              snap.screen())


def test_a_state_can_carry_a_chrome_chain():
    """The point of the feature: a border that says what a pane is doing."""
    conf = cfg('states {\n'
               '    unfocused { tint color="#ff0000" amount=255 where="chrome" }\n'
               '}\n')
    with Session(SH, cols=90, rows=14, config=conf) as s:
        s.until_text("BLOCK")
        s.key("\\\\")
        s.settle()
        snap = s.snapshot()
        panes = s.panes()
        focused = [p for p in panes if p["focused"]][0]
        other = [p for p in panes if not p["focused"]][0]
        check("the pane you are not in wears it",
              frame_fg(snap, other) == "#ff0000", str(frame_fg(snap, other)))
        check("the one you are in does not",
              frame_fg(snap, focused) != "#ff0000", str(frame_fg(snap, focused)))

        s.key("o")  # focus the next pane: the states swap
        s.settle()
        snap = s.snapshot()
        check("and it follows focus rather than sticking to a pane",
              frame_fg(snap, focused) == "#ff0000" and
              frame_fg(snap, other) != "#ff0000",
              f"{frame_fg(snap, focused)} / {frame_fg(snap, other)}")


def test_a_state_block_still_replaces_the_whole_state():
    """A block holding only chrome passes is a state with no content pass, and
    `unfocused` is the one where that is worth knowing: naming it takes the
    dim_unfocused knob out of play, chrome chain or not. Documented in
    config.kdl, and checked here so the documentation cannot drift from it."""
    conf = cfg('states {\n'
               '    unfocused { tint color="#ff0000" amount=255 where="chrome" }\n'
               '}\n')
    with Session(SH, cols=90, rows=14, config=conf) as s:
        s.until_text("BLOCK")
        s.key("\\\\")
        s.settle()
        snap = s.snapshot()
        other = [p for p in s.panes() if not p["focused"]][0]
        check("the unfocused pane's contents are left alone",
              content_fg(snap, other) == GREEN, str(content_fg(snap, other)))


def test_a_reload_puts_a_chrome_chain_on_and_takes_it_off_again():
    """The way anyone will actually try this: save the config and look. Taking
    it off again is the half worth testing — a chain that survived its own
    removal would be a session you could not get back."""
    path = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    path.write(THEME)
    path.close()

    def reload_with(s, text):
        with open(path.name, "w") as f:
            f.write(THEME + text)
        s.api("reload")
        s.settle(30)

    with Session(SH, cols=60, rows=12, config=path.name) as s:
        s.until_text("BLOCK")
        check("the frame starts in its own colour",
              frame_fg(s.snapshot(), s.pane()) == FRAME,
              str(frame_fg(s.snapshot(), s.pane())))

        reload_with(s, 'shaders {\n'
                       '    tint color="#ff0000" amount=255 where="chrome"\n'
                       '}\n')
        check("saving a chrome pass colours the frame",
              frame_fg(s.snapshot(), s.pane()) == "#ff0000",
              str(frame_fg(s.snapshot(), s.pane())))

        reload_with(s, "")
        check("and taking it out of the file takes it off the frame",
              frame_fg(s.snapshot(), s.pane()) == FRAME,
              str(frame_fg(s.snapshot(), s.pane())))
    os.unlink(path.name)


def bg_at(snap, x, y):
    """None when the cell has no background of ours — which is what a frame
    cell normally is: the terminal draws it in whatever its default is."""
    run = snap.style_at(x, y)
    return run.get("bg") if run else None


def test_a_pass_can_be_told_to_leave_the_background_alone():
    """`tint` mixes both colours, and a frame's background is usually the
    terminal's own default — so a border tint painted a dark rectangle behind
    the glyphs and left it there. `channel="fg"` is the fix, and the background
    has to come back *unset* rather than as our idea of what default means."""
    both = cfg('shaders {\n'
               '    tint color="#ff0000" amount=200 where="chrome"\n'
               '}\n')
    fg_only = cfg('shaders {\n'
                  '    tint color="#ff0000" amount=200 where="chrome" channel="fg"\n'
                  '}\n')
    with Session(SH, cols=60, rows=12, config=both) as s:
        s.until_text("BLOCK")
        p, snap = s.pane(), s.snapshot()
        check("without a channel the background is painted too",
              bg_at(snap, p["x"] + 5, p["y"]) is not None,
              str(bg_at(snap, p["x"] + 5, p["y"])))

    with Session(SH, cols=60, rows=12, config=fg_only) as s:
        s.until_text("BLOCK")
        p, snap = s.pane(), s.snapshot()
        check("channel fg still recolours the border glyphs",
              frame_fg(snap, p) not in (FRAME, None), str(frame_fg(snap, p)))
        check("and the frame keeps the terminal's own background",
              bg_at(snap, p["x"] + 5, p["y"]) is None,
              str(bg_at(snap, p["x"] + 5, p["y"])))
        check("the contents are still untouched",
              content_fg(snap, p) == GREEN, str(content_fg(snap, p)))


def test_an_unknown_channel_is_refused_not_guessed():
    conf = cfg('shaders {\n'
               '    tint color="#ff0000" amount=255 where="chrome" channel="foreground"\n'
               '}\n')
    with Session(SH, cols=60, rows=12, config=conf) as s:
        s.until_text("BLOCK")
        p, snap = s.pane(), s.snapshot()
        check("the session runs and nothing was shaded",
              s.alive() and frame_fg(snap, p) == FRAME and
              content_fg(snap, p) == GREEN,
              f"{frame_fg(snap, p)} / {content_fg(snap, p)}")


def ring(s, pane_id, back_to):
    """Ring the bell in a pane while looking at another one, which is the only
    case a bell is for: looking at a pane answers it."""
    s.api("focus", id=pane_id)
    s.settle(10)
    s.raw(r"\x07")
    s.settle(30)
    s.api("focus", id=back_to)
    s.settle(30)


def test_a_bell_can_flash_the_border():
    """The bell state plus `since`, which is the pair that makes a *flash*
    expressible: `t` says what time it is, not how long ago the thing rang."""
    # One node per line: a newline ends a KDL node, so a pass split over two
    # lines is two broken nodes rather than one pass.
    conf = cfg('anim_ms 20\n'
               'states {\n'
               '    bell { tint where="chrome" color="#ff0000" amount="(since < 250) * 255" }\n'
               '}\n')
    with Session(SH, cols=90, rows=14, config=conf) as s:
        s.until_text("BLOCK")
        s.key("\\\\")
        s.settle()
        mine = [p for p in s.panes() if p["focused"]][0]
        other = [p for p in s.panes() if not p["focused"]][0]

        check("nothing flashes before anything rings",
              frame_fg(s.snapshot(), other) == DIM_FRAME,
              str(frame_fg(s.snapshot(), other)))

        ring(s, other["id"], mine["id"])
        check("the pane that rang flashes",
              frame_fg(s.snapshot(), other) == "#ff0000",
              str(frame_fg(s.snapshot(), other)))
        check("and asks for the frames to animate it",
              s.deadline() == 20, str(s.deadline()))
        check("the pane you are in does not",
              frame_fg(s.snapshot(), mine) == FRAME,
              str(frame_fg(s.snapshot(), mine)))

        # Real elapsed time: `since` is a clock, and the flash is defined as
        # ending on its own.
        time.sleep(0.3)
        check("the flash ends by itself, without the bell being answered",
              frame_fg(s.snapshot(), other) == DIM_FRAME,
              str(frame_fg(s.snapshot(), other)))


def test_the_bell_state_ends_when_you_look_at_the_pane():
    """Answering a bell is looking at it, so the colour has to go then too —
    a border still shouting about a bell you have read is worse than none."""
    conf = cfg('states {\n'
               '    bell { tint where="chrome" color="#ff0000" amount=255 }\n'
               '}\n')
    with Session(SH, cols=90, rows=14, config=conf) as s:
        s.until_text("BLOCK")
        s.key("\\\\")
        s.settle()
        mine = [p for p in s.panes() if p["focused"]][0]
        other = [p for p in s.panes() if not p["focused"]][0]

        ring(s, other["id"], mine["id"])
        check("it holds while nobody has looked",
              frame_fg(s.snapshot(), other) == "#ff0000",
              str(frame_fg(s.snapshot(), other)))

        s.api("focus", id=other["id"])
        s.settle()
        check("and stops the moment you do",
              frame_fg(s.snapshot(), other) == FRAME,
              str(frame_fg(s.snapshot(), other)))


def test_a_rung_pane_is_rung_rather_than_merely_unfocused():
    """States do not stack, and of the two things true of a pane that rang
    while you were elsewhere, the ringing is the one you needed telling."""
    conf = cfg('states {\n'
               '    unfocused { dim where="chrome" amount=255 }\n'
               '    bell { tint where="chrome" color="#ff0000" amount=255 }\n'
               '}\n')
    with Session(SH, cols=90, rows=14, config=conf) as s:
        s.until_text("BLOCK")
        s.key("\\\\")
        s.settle()
        mine = [p for p in s.panes() if p["focused"]][0]
        other = [p for p in s.panes() if not p["focused"]][0]
        check("an unfocused pane wears the unfocused chain",
              frame_fg(s.snapshot(), other) == "#000000",
              str(frame_fg(s.snapshot(), other)))

        ring(s, other["id"], mine["id"])
        check("and the bell chain replaces it once it rings",
              frame_fg(s.snapshot(), other) == "#ff0000",
              str(frame_fg(s.snapshot(), other)))


def test_a_collapsed_pane_is_chrome_all_the_way_through():
    """Once a tab flattens (D6) a pane *is* one row of frame, and that row is
    the only thing saying the pane exists — so a chrome pass has to reach it."""
    conf = cfg('shaders {\n    tint color="#ff0000" amount=255 where="chrome"\n}\n')
    with Session(SH, cols=60, rows=24, config=conf) as s:
        s.until_text("BLOCK")
        s.key("-")
        s.key("-")
        s.settle()
        s.resize(60, 12)  # too small for three panes: the tab becomes a list
        s.settle()
        rows = [p for p in s.panes() if p["h"] == 1]
        check("the tab flattened", len(rows) >= 1, str(s.panes()))
        snap = s.snapshot()
        check("a collapsed row takes the chrome colour",
              fg_at(snap, rows[0]["x"] + 2, rows[0]["y"]) == "#ff0000",
              snap.screen())


def test_an_unknown_where_is_refused_not_guessed():
    """Running it over the contents because "chrom" was a typo would be a
    surprise; the entry said where it wanted to be."""
    conf = cfg('shaders {\n    tint color="#ff0000" amount=255 where="chrom"\n}\n')
    with Session(SH, cols=60, rows=12, config=conf) as s:
        s.until_text("BLOCK")
        p, snap = s.pane(), s.snapshot()
        check("the session still runs", s.alive())
        check("and neither the frame nor the contents were shaded",
              frame_fg(snap, p) == FRAME and content_fg(snap, p) == GREEN,
              f"{frame_fg(snap, p)} / {content_fg(snap, p)}")


def test_an_animated_chrome_pass_asks_for_a_frame_clock():
    conf = cfg('anim_ms 40\n'
               'shaders {\n'
               '    tint color="#00ff00" amount="((t / 50) % 2) * 255" where="chrome"\n'
               '}\n')
    with Session(SH, cols=60, rows=12, config=conf) as s:
        s.until_text("BLOCK")
        s.snapshot()
        check("the session wants repainting on the clock it was given",
              s.deadline() == 40, str(s.deadline()))

        # Real elapsed time: the thing under test is a clock, and the only
        # honest way to ask whether a colour moves is to wait and look.
        seen = set()
        for _ in range(8):
            time.sleep(0.03)
            seen.add(frame_fg(s.snapshot(), s.pane()))
        check("and the border actually moves", len(seen) > 1, str(seen))


def test_a_still_shader_asks_for_nothing():
    """The clock is opt-in by construction: it comes from the chain that ran,
    so a session with no animated shader keeps its idle poll."""
    conf = cfg('shaders {\n    tint color="#ff0000" amount=255 where="chrome"\n}\n')
    with Session(SH, cols=60, rows=12, config=conf) as s:
        s.until_text("BLOCK")
        s.snapshot()
        check("nothing is due", s.deadline() == -1, str(s.deadline()))


if __name__ == "__main__":
    test_a_chrome_pass_colours_the_frame_and_nothing_else()
    test_a_content_pass_still_leaves_the_frame_alone()
    test_the_two_passes_can_run_together()
    test_positions_are_the_whole_frames()
    test_a_state_can_carry_a_chrome_chain()
    test_a_state_block_still_replaces_the_whole_state()
    test_a_reload_puts_a_chrome_chain_on_and_takes_it_off_again()
    test_a_pass_can_be_told_to_leave_the_background_alone()
    test_an_unknown_channel_is_refused_not_guessed()
    test_a_bell_can_flash_the_border()
    test_the_bell_state_ends_when_you_look_at_the_pane()
    test_a_rung_pane_is_rung_rather_than_merely_unfocused()
    test_a_collapsed_pane_is_chrome_all_the_way_through()
    test_an_unknown_where_is_refused_not_guessed()
    test_an_animated_chrome_pass_asks_for_a_frame_clock()
    test_a_still_shader_asks_for_nothing()
    sys.exit(report())
