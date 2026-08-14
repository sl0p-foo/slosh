#!/usr/bin/env python3
"""Kitty graphics passthrough.

libghostty-vt parses the protocol and tracks the images; what a multiplexer
has to add is re-emitting them to the client at the right place, with ids that
cannot collide between panes. That collision is the whole reason tmux and
zellij drop images instead, and it is the first thing tested here.
"""
import base64
import sys

from harness import Session, check, report

# a 4x2 RGB image
PX = base64.b64encode(bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0] * 2)).decode()


def sends_image(image_id=7, cols=6, rows=2, after="sleep 5"):
    seq = f"\\033_Ga=T,f=24,s=4,v=2,i={image_id},q=2,c={cols},r={rows};{PX}\\033\\\\"
    return ["/bin/sh", "-c", f'stty raw -echo; printf "{seq}"; {after}']


def places(s):
    return s.api("graphics")["placements"]


def transmits_then_places(place_keys, before_place="", image_id=7):
    """The other half of the protocol: upload once with `a=t`, place later
    with `a=p`. Everything a program that draws repeatedly does."""
    t = f"\\033_Ga=t,f=24,s=4,v=2,i={image_id},q=2;{PX}\\033\\\\"
    p = f"\\033_Ga={place_keys}\\033\\\\"
    return ["/bin/sh", "-c",
            f'stty raw -echo; printf "{t}"; printf "{before_place}"; '
            f'printf "{p}"; sleep 5']


def test_a_pane_image_reaches_the_screen():
    with Session(sends_image(), cols=44, rows=10) as s:
        s.settle(200)
        p = s.pane()
        pl = places(s)
        check("the image is placed", len(pl) == 1, str(pl))
        if not pl:
            return
        check("at the pane's content origin, in screen cells",
              (pl[0]["x"], pl[0]["y"]) == (p["content_x"], p["content_y"]),
              f"{pl[0]} vs content {p['content_x']},{p['content_y']}")
        check("with the size the program asked for",
              (pl[0]["cols"], pl[0]["rows"]) == (6, 2), str(pl[0]))


def test_a_placement_that_does_not_say_how_big_it_is():
    """`c=`/`r=` are optional: without them the image covers as many cells as
    its pixels need, which the terminal can only work out if it knows how big
    a cell is. We were passing 0 -- so every such image covered no cells and
    silently did not appear, which is most of what "kitty graphics works"
    was worth."""
    with Session(transmits_then_places("p,i=7,p=1,q=2"), cols=44, rows=10) as s:
        s.settle(200)
        pl = places(s)
        check("it is placed", len(pl) == 1, str(pl))
        if not pl:
            return
        # 4x2 pixels at the default 8x16 cell: one cell each way.
        check("sized from its pixels and the cell size",
              (pl[0]["cols"], pl[0]["rows"]) == (1, 1), str(pl[0]))


def test_the_cell_size_a_client_reports_is_what_sizes_an_image():
    with Session(transmits_then_places("p,i=7,p=1,q=2"), cols=44, rows=10) as s:
        s.settle(200)
        s.api("resize", cols=44, rows=10, cell_w=2, cell_h=1)
        s.settle(150)
        pl = places(s)
        # The same 4x2 image against a 2x1 cell is 2 cells wide and 2 tall.
        check("a different cell means a different number of cells",
              pl and (pl[0]["cols"], pl[0]["rows"]) == (2, 2), str(pl))


def test_a_program_can_read_the_pixel_size_from_its_pty():
    """TIOCGWINSZ has pixel fields, and a program that draws images reads
    them. Zeroes there are why dvd.py fell back to guessing 8x16."""
    prog = ("import fcntl, termios, struct, sys, time;"
            "r, c, xp, yp = struct.unpack('HHHH', "
            "fcntl.ioctl(0, termios.TIOCGWINSZ, b'\\0' * 8));"
            "print('WS', c, r, xp, yp); sys.stdout.flush(); time.sleep(5)")
    with Session(["python3", "-c", prog], cols=50, rows=10) as s:
        snap = s.until_text("WS ")
        line = [l for l in snap.text if "WS " in l][0].strip()
        cols, rows, xp, yp = (int(v) for v in line.split()[1:5])
        check("the pty carries pixel dimensions", xp and yp, line)
        check("consistent with the cell size we were told",
              xp == cols * 8 and yp == rows * 16, line)


def test_an_image_outlives_a_screen_clear():
    """A program transmits once and places every frame, and full-screen
    programs clear the screen. libghostty-vt freed the image data on ED(2),
    so every placement after the first clear drew nothing -- see
    vendor/patches. The placements go, the image stays."""
    with Session(transmits_then_places("p,i=7,p=1,q=2,c=6,r=2",
                                       before_place="\\033[2J"),
                 cols=44, rows=10) as s:
        s.settle(200)
        check("a clear before the placement does not lose the image",
              len(places(s)) == 1, str(places(s)))


def test_a_screen_clear_still_removes_what_is_on_screen():
    argv = ["/bin/sh", "-c",
            f'stty raw -echo; printf "\\033_Ga=T,f=24,s=4,v=2,i=7,q=2,c=6,r=2;{PX}\\033\\\\"; '
            f'sleep 0.2; printf "\\033[2J"; printf CLEARED; sleep 5']
    with Session(argv, cols=44, rows=10) as s:
        # Both waits are on something observable: the image arrives, then the
        # clear that follows it. A settle would race the sleep between them.
        s.until(lambda _: len(places(s)) == 1)
        check("placed to begin with", len(places(s)) == 1, str(places(s)))
        s.until_text("CLEARED")
        check("the placement is gone with the screen it was on",
              len(places(s)) == 0, str(places(s)))


def test_sub_cell_offsets_survive_to_the_client():
    """A program that moves something smoothly places it part-way into a cell
    with X=/Y=. We tracked the placement and dropped the offsets, so anything
    moving jumped a whole cell at a time -- invisible in a still picture and
    the first thing you see when it moves."""
    with Session(transmits_then_places("p,i=7,p=1,q=2,c=6,r=2,X=3,Y=5"),
                 cols=44, rows=10) as s:
        s.settle(200)
        pl = places(s)
        check("the offsets are tracked", pl and (pl[0]["x_off"], pl[0]["y_off"]) == (3, 5),
              str(pl))

        # And, separately, that they reach the terminal: the model being right
        # is not the same as the bytes being right.
        raw = s.api("graphics", format="bytes")["bytes"]
        place = [c for c in raw.split("\x1b") if c.startswith("_Ga=p")]
        check("and emitted to the client", place and "X=3" in place[0] and
              "Y=5" in place[0], str(place))


def test_a_placement_with_no_offset_emits_none():
    with Session(transmits_then_places("p,i=7,p=1,q=2,c=6,r=2"),
                 cols=44, rows=10) as s:
        s.settle(200)
        raw = s.api("graphics", format="bytes")["bytes"]
        place = [c for c in raw.split("\x1b") if c.startswith("_Ga=p")]
        check("nothing is invented", place and "X=" not in place[0] and
              "Y=" not in place[0], str(place))


def test_a_natural_image_is_never_rescaled_as_it_moves():
    """`c=`/`r=` mean *scale into this many cells*. Passing on the count a
    natural-size image happens to cover is invisible in a still picture and
    wrong the moment it moves: the count goes up by one whenever the image
    straddles one more cell boundary, so the picture changes size."""
    with Session(transmits_then_places("p,i=7,p=1,q=2,X=3,Y=5"),
                 cols=44, rows=10) as s:
        s.settle(200)
        raw = s.api("graphics", format="bytes")["bytes"]
        place = [c for c in raw.split("\x1b") if c.startswith("_Ga=p")]
        check("no cell count is sent for a natural placement",
              place and "c=" not in place[0] and "r=" not in place[0],
              str(place))


def test_a_scaled_image_keeps_the_cell_count_it_asked_for():
    with Session(transmits_then_places("p,i=7,p=1,q=2,c=6,r=2"),
                 cols=44, rows=10) as s:
        s.settle(200)
        raw = s.api("graphics", format="bytes")["bytes"]
        place = [c for c in raw.split("\x1b") if c.startswith("_Ga=p")]
        check("what the program asked for is passed on",
              place and "c=6" in place[0] and "r=2" in place[0], str(place))


def test_ids_cannot_collide_between_panes():
    """Two panes, both using image id 7. They must not become one image."""
    with Session(sends_image(image_id=7), cols=90, rows=12) as s:
        s.settle(200)
        s.key("\\\\")
        s.settle(250)
        pl = places(s)
        check("both panes place an image", len(pl) == 2, str(pl))
        if len(pl) != 2:
            return
        check("and they were given different ids for the client",
              pl[0]["image"] != pl[1]["image"], str(pl))
        check("side by side, where their panes are",
              pl[0]["x"] != pl[1]["x"], str(pl))


def test_placement_follows_the_layout():
    with Session(sends_image(), cols=90, rows=12) as s:
        s.settle(200)
        before = places(s)[0]
        s.key("\\\\")          # the pane with the image is now the left half
        s.settle(250)
        s.api("focus", id=s.panes()[0]["id"])
        s.settle(80)
        after = [q for q in places(s) if q["image"] == before["image"]]
        check("the placement is still tracked after a split", after != [],
              str(places(s)))


def test_cropped_at_the_pane_edge():
    with Session(sends_image(cols=40, rows=8), cols=40, rows=10) as s:
        s.settle(200)
        p = s.pane()
        pl = places(s)
        check("an oversized image is placed", len(pl) == 1, str(pl))
        if not pl:
            return
        check("cropped to the columns the pane has",
              pl[0]["cols"] <= p["content_w"], f"{pl[0]} vs {p['content_w']}")
        check("and to its rows",
              pl[0]["rows"] <= p["content_h"], f"{pl[0]} vs {p['content_h']}")


def test_hidden_panes_place_nothing():
    with Session(sends_image(), cols=120, rows=30) as s:
        s.settle(200)
        s.key("\\\\")
        s.settle(150)
        s.key("\\\\")
        s.settle(200)
        check("three panes, one with an image", len(places(s)) == 3, str(places(s)))
        s.resize(40, 12)  # collapses
        s.settle(200)
        hidden = [q for q in s.panes() if q["hidden"]]
        check("something collapsed", hidden != [], str(s.panes()))
        check("a collapsed pane places nothing",
              len(places(s)) == len(s.panes()) - len(hidden), str(places(s)))


def test_placement_goes_away_with_its_pane():
    with Session(sends_image(), cols=90, rows=12) as s:
        s.settle(200)
        s.key("\\\\")
        s.settle(250)
        check("two placements", len(places(s)) == 2, str(places(s)))
        s.key("x")  # close the focused pane
        s.settle(200)
        check("closing a pane removes its placement", len(places(s)) == 1,
              str(places(s)))


def test_partial_visibility_crops_rather_than_squashes():
    """An image taller than its pane is cropped by moving the source rect.

    Asking the terminal for fewer rows alone would *scale* the image into
    them, which is not what "the rest is off-screen" looks like.
    """
    with Session(sends_image(cols=10, rows=20), cols=44, rows=12) as s:
        s.settle(250)
        p = s.pane()
        pl = places(s)
        check("a taller-than-the-pane image is still placed", len(pl) == 1,
              str(pl))
        if not pl:
            return
        check("clipped to the pane's rows", pl[0]["rows"] <= p["content_h"],
              f"{pl[0]} vs {p['content_h']}")


def test_scrolled_away_placements_are_dropped():
    prog = ("stty raw -echo; "
            f'printf "\\033_Ga=T,f=24,s=4,v=2,i=7,q=2,c=6,r=2;{PX}\\033\\\\"; '
            "seq 1 200; cat")
    with Session(["/bin/sh", "-c", prog], cols=44, rows=10) as s:
        s.settle(400)
        check("an image scrolled out of the viewport is not placed",
              places(s) == [], str(places(s)))


if __name__ == "__main__":
    test_a_pane_image_reaches_the_screen()
    test_a_placement_that_does_not_say_how_big_it_is()
    test_the_cell_size_a_client_reports_is_what_sizes_an_image()
    test_a_program_can_read_the_pixel_size_from_its_pty()
    test_an_image_outlives_a_screen_clear()
    test_a_screen_clear_still_removes_what_is_on_screen()
    test_sub_cell_offsets_survive_to_the_client()
    test_a_placement_with_no_offset_emits_none()
    test_a_natural_image_is_never_rescaled_as_it_moves()
    test_a_scaled_image_keeps_the_cell_count_it_asked_for()
    test_ids_cannot_collide_between_panes()
    test_placement_follows_the_layout()
    test_cropped_at_the_pane_edge()
    test_partial_visibility_crops_rather_than_squashes()
    test_hidden_panes_place_nothing()
    test_placement_goes_away_with_its_pane()
    test_scrolled_away_placements_are_dropped()
    sys.exit(report())
