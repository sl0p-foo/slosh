#!/usr/bin/env python3
"""The two things that drive shaders: focus, and a drag in progress.

Both are derived from the current frame rather than attached and remembered,
so most of what is worth checking is that they turn *off* again — a pane left
grey by a path that forgot to clear it is the failure mode this design exists
to make impossible.

Colours are asserted through the style runs the snapshot already reports, so
these are real end-to-end checks: a program's output, composed, shaded, and
read back the way a client would see it.
"""
import sys
import tempfile

from harness import Session, check, report

# A pane that paints a known, non-default colour into its own content.
GREEN = "#00ff00"
SH = ["/bin/sh", "-c",
      'printf "\\033]2;p\\007"; printf "\\033[38;2;0;255;0mBLOCK\\033[0m\\n"; '
      'stty raw -echo; cat']


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def content_fg(snap, pane):
    """The colour of the pane's own first content cell."""
    run = snap.style_at(pane["content_x"], pane["content_y"])
    return (run or {}).get("fg")


def frame_fg(snap, pane):
    run = snap.style_at(pane["x"], pane["y"])
    return (run or {}).get("fg")


def split(s):
    s.key("\\\\")
    s.settle()
    return s.panes()


def by_focus(s):
    panes = s.panes()
    focused = [p for p in panes if p["focused"]][0]
    other = [p for p in panes if not p["focused"]][0]
    return focused, other


def press(s, x, y):
    s.send(rf"\e[<0;{x + 1};{y + 1}M")


def motion(s, x, y):
    s.send(rf"\e[<32;{x + 1};{y + 1}M")


def release(s, x, y):
    s.send(rf"\e[<0;{x + 1};{y + 1}m")


# ---- dim_unfocused ---------------------------------------------------------

def test_off_by_default():
    with Session(SH, cols=80, rows=14) as s:
        s.settle(200)
        split(s)
        s.settle(200)
        focused, other = by_focus(s)
        snap = s.snapshot()
        check("by default an unfocused pane is not dimmed",
              content_fg(snap, other) == GREEN, str(content_fg(snap, other)))
        check("and the focused one is not either",
              content_fg(snap, focused) == GREEN,
              str(content_fg(snap, focused)))


def test_dims_everything_but_the_focused_pane():
    with Session(SH, cols=80, rows=14, config=cfg("dim_unfocused 128\n")) as s:
        s.settle(200)
        split(s)
        s.settle(200)
        focused, other = by_focus(s)
        snap = s.snapshot()
        check("the focused pane keeps its colour",
              content_fg(snap, focused) == GREEN,
              str(content_fg(snap, focused)))
        check("an unfocused pane is dimmed",
              content_fg(snap, other) not in (GREEN, None),
              str(content_fg(snap, other)))
        check("dimmed means darker, not merely different",
              int(content_fg(snap, other)[3:5], 16) < 0xff,
              str(content_fg(snap, other)))


def test_the_dimming_follows_focus():
    with Session(SH, cols=80, rows=14, config=cfg("dim_unfocused 128\n")) as s:
        s.settle(200)
        split(s)
        s.settle(200)
        first_focused, first_other = by_focus(s)

        s.api("focus", id=first_other["id"])
        s.settle(200)
        snap = s.snapshot()
        now_focused = [p for p in s.panes() if p["id"] == first_other["id"]][0]
        now_other = [p for p in s.panes() if p["id"] == first_focused["id"]][0]
        check("the newly focused pane is undimmed",
              content_fg(snap, now_focused) == GREEN,
              str(content_fg(snap, now_focused)))
        check("and the one that lost focus is dimmed now",
              content_fg(snap, now_other) not in (GREEN, None),
              str(content_fg(snap, now_other)))


def test_dimming_never_touches_the_chrome():
    with Session(SH, cols=80, rows=14, config=cfg("dim_unfocused 200\n")) as s:
        s.settle(200)
        split(s)
        s.settle(200)
        _, other = by_focus(s)
        snap = s.snapshot()
        check("a dimmed pane's frame keeps its full colour",
              frame_fg(snap, other) == "#45454a", str(frame_fg(snap, other)))
        check("and the tab strip is untouched",
              "#" in str(snap.style_at(3, 1)), str(snap.style_at(3, 1)))


# ---- drag_grayscale --------------------------------------------------------

def test_dragging_greys_the_other_panes():
    with Session(SH, cols=90, rows=16) as s:
        s.settle(200)
        left, right = split(s)
        s.settle(200)

        press(s, left["x"] + 4, left["y"])
        motion(s, right["x"] + 5, right["y"] + 3)
        s.settle(120)
        snap = s.snapshot()
        check("the pane being dragged keeps its colour",
              content_fg(snap, left) == GREEN, str(content_fg(snap, left)))
        check("every other pane goes grey",
              content_fg(snap, right) not in (GREEN, None),
              str(content_fg(snap, right)))
        # The default strength is 200, not 255: a partial desaturation that
        # keeps a trace of the original hue. So the test is that the channels
        # have converged, not that they are equal.
        grey = content_fg(snap, right)
        ch = [int(grey[i:i + 2], 16) for i in (1, 3, 5)]
        check("the colour has collapsed towards grey",
              max(ch) - min(ch) < 80, f"{grey} spread={max(ch) - min(ch)}")
        check("the two channels that started equal stay equal",
              ch[0] == ch[2], str(grey))

        release(s, right["x"] + 5, right["y"] + 3)
        s.settle(200)
        snap = s.snapshot()
        for p in s.panes():
            check(f"pane {p['id']} is back to its own colour after the drop",
                  content_fg(snap, p) == GREEN, str(content_fg(snap, p)))


def test_full_strength_is_a_true_grey():
    with Session(SH, cols=90, rows=16,
                 config=cfg("drag_grayscale 255\n")) as s:
        s.settle(200)
        left, right = split(s)
        s.settle(200)

        press(s, left["x"] + 4, left["y"])
        motion(s, right["x"] + 5, right["y"] + 3)
        s.settle(120)
        grey = content_fg(s.snapshot(), right)
        check("at 255 every channel agrees exactly",
              grey and grey[1:3] == grey[3:5] == grey[5:7], str(grey))
        release(s, right["x"] + 5, right["y"] + 3)
        s.settle(120)


def test_a_press_that_never_moves_greys_nothing():
    """A click on a title is a click. It must not flash the session grey."""
    with Session(SH, cols=90, rows=16) as s:
        s.settle(200)
        left, right = split(s)
        s.settle(200)

        press(s, left["x"] + 4, left["y"])
        s.settle(120)
        snap = s.snapshot()
        check("pressing without moving greys nothing",
              content_fg(snap, right) == GREEN, str(content_fg(snap, right)))
        release(s, left["x"] + 4, left["y"])
        s.settle(120)


def test_the_drag_greying_outranks_the_focus_dimming():
    """Two reasons to be grey would compound into one muddy grey."""
    with Session(SH, cols=90, rows=16, config=cfg("dim_unfocused 128\n")) as s:
        s.settle(200)
        left, right = split(s)
        s.settle(200)

        # Drag the *unfocused* pane, so focus policy would dim it if it applied.
        focused, other = by_focus(s)
        press(s, other["x"] + 4, other["y"])
        motion(s, focused["x"] + 5, focused["y"] + 3)
        s.settle(120)
        snap = s.snapshot()
        check("the dragged pane lifts off the page, dimming or not",
              content_fg(snap, other) == GREEN, str(content_fg(snap, other)))
        release(s, focused["x"] + 5, focused["y"] + 3)
        s.settle(200)


def drag_and_sample(conf):
    """Colour of a non-dragged pane's content mid-drag, under `conf`."""
    with Session(SH, cols=90, rows=16, config=cfg(conf)) as s:
        s.settle(200)
        left, right = split(s)
        s.settle(200)
        press(s, left["x"] + 4, left["y"])
        motion(s, right["x"] + 5, right["y"] + 3)
        s.settle(120)
        got = content_fg(s.snapshot(), right)
        release(s, right["x"] + 5, right["y"] + 3)
        s.settle(120)
        return got


def test_the_two_drag_knobs_are_independent():
    both_off = drag_and_sample("drag_grayscale 0\ndrag_dim 0\n")
    check("with both off a drag leaves every pane alone", both_off == GREEN,
          str(both_off))

    # Desaturate only: channels converge, brightness roughly survives.
    gray_only = drag_and_sample("drag_grayscale 255\ndrag_dim 0\n")
    ch = [int(gray_only[i:i + 2], 16) for i in (1, 3, 5)]
    check("grayscale alone desaturates without darkening much",
          ch[0] == ch[1] == ch[2] and ch[1] > 0x60, str(gray_only))

    # Darken only: the hue survives, the brightness does not.
    dim_only = drag_and_sample("drag_grayscale 0\ndrag_dim 200\n")
    ch = [int(dim_only[i:i + 2], 16) for i in (1, 3, 5)]
    check("dim alone darkens without desaturating",
          ch[0] == 0 and ch[2] == 0 and 0 < ch[1] < 0x60, str(dim_only))


def test_the_default_drag_look_is_grey_and_recessed():
    """Both together: the dragged pane has to win on contrast, not just hue."""
    got = drag_and_sample("")
    ch = [int(got[i:i + 2], 16) for i in (1, 3, 5)]
    check("a non-dragged pane is pushed well below full brightness",
          max(ch) < 0x80, f"{got} max={max(ch):#x}")
    check("and towards grey", max(ch) - min(ch) < 60,
          f"{got} spread={max(ch) - min(ch)}")


# ---- the drop-target border (chrome, not a shader) -------------------------

DASH_H = "\u2504"
SOLID_H = "\u2500"


def test_non_dragged_panes_get_a_dashed_border():
    with Session(SH, cols=90, rows=16) as s:
        s.settle(200)
        left, right = split(s)
        s.settle(200)

        snap = s.snapshot()
        check("borders are solid when nothing is being dragged",
              DASH_H not in snap.line(right["y"]), repr(snap.line(right["y"])))

        press(s, left["x"] + 4, left["y"])
        motion(s, right["x"] + 5, right["y"] + 3)
        s.settle(120)
        snap = s.snapshot()
        top = snap.line(right["y"])[right["x"]:right["x"] + right["w"]]
        dragged_top = snap.line(left["y"])[left["x"]:left["x"] + left["w"]]
        check("a pane you could drop onto is dashed", DASH_H in top, repr(top))
        check("the pane in your hand stays solid",
              DASH_H not in dragged_top and SOLID_H in dragged_top,
              repr(dragged_top))

        release(s, right["x"] + 5, right["y"] + 3)
        s.settle(200)
        snap = s.snapshot()
        for p in s.panes():
            row = snap.line(p["y"])[p["x"]:p["x"] + p["w"]]
            check(f"pane {p['id']} is solid again after the drop",
                  DASH_H not in row, repr(row))


def test_a_press_that_never_moves_dashes_nothing():
    with Session(SH, cols=90, rows=16) as s:
        s.settle(200)
        left, right = split(s)
        s.settle(200)
        press(s, left["x"] + 4, left["y"])
        s.settle(120)
        snap = s.snapshot()
        check("pressing without moving dashes nothing",
              DASH_H not in snap.screen(), "")
        release(s, left["x"] + 4, left["y"])
        s.settle(120)


def test_the_hovered_target_is_still_highlighted_over_the_dashes():
    with Session(SH, cols=90, rows=16) as s:
        s.settle(200)
        left, right = split(s)
        s.settle(200)
        press(s, left["x"] + 4, left["y"])
        motion(s, right["x"] + 5, right["y"] + 3)
        s.settle(120)
        snap = s.snapshot()
        run = snap.style_at(right["x"], right["y"])
        check("the pane under the pointer keeps its drop highlight",
              run and run["fg"] == "#ff5fd7" and "bold" in run["attrs"],
              str(run))
        release(s, right["x"] + 5, right["y"] + 3)
        s.settle(120)


def test_a_drag_that_ends_off_a_pane_still_clears():
    """The clear is derived, so even a drop into nowhere cannot leave it on."""
    with Session(SH, cols=90, rows=16) as s:
        s.settle(200)
        left, right = split(s)
        s.settle(200)

        press(s, left["x"] + 4, left["y"])
        motion(s, right["x"] + 5, right["y"] + 3)
        s.settle(120)
        motion(s, 0, 0)          # out into the margin
        release(s, 0, 0)
        s.settle(200)
        snap = s.snapshot()
        for p in s.panes():
            check(f"pane {p['id']} is un-greyed after a drop into nowhere",
                  content_fg(snap, p) == GREEN, str(content_fg(snap, p)))


def test_a_keystroke_mid_drag_clears_the_greying():
    """Any key ends a drag (so the mouse cannot wedge); colour must follow."""
    with Session(SH, cols=90, rows=16) as s:
        s.settle(200)
        left, right = split(s)
        s.settle(200)

        press(s, left["x"] + 4, left["y"])
        motion(s, right["x"] + 5, right["y"] + 3)
        s.settle(120)
        s.send("x")
        s.settle(200)
        snap = s.snapshot()
        check("the greying goes with the drag it belonged to",
              content_fg(snap, right) == GREEN, str(content_fg(snap, right)))


if __name__ == "__main__":
    test_off_by_default()
    test_dims_everything_but_the_focused_pane()
    test_the_dimming_follows_focus()
    test_dimming_never_touches_the_chrome()
    test_dragging_greys_the_other_panes()
    test_full_strength_is_a_true_grey()
    test_a_press_that_never_moves_greys_nothing()
    test_the_drag_greying_outranks_the_focus_dimming()
    test_the_two_drag_knobs_are_independent()
    test_the_default_drag_look_is_grey_and_recessed()
    test_non_dragged_panes_get_a_dashed_border()
    test_a_press_that_never_moves_dashes_nothing()
    test_the_hovered_target_is_still_highlighted_over_the_dashes()
    test_a_drag_that_ends_off_a_pane_still_clears()
    test_a_keystroke_mid_drag_clears_the_greying()
    sys.exit(report())
