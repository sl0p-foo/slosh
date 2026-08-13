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
    test_ids_cannot_collide_between_panes()
    test_placement_follows_the_layout()
    test_cropped_at_the_pane_edge()
    test_partial_visibility_crops_rather_than_squashes()
    test_hidden_panes_place_nothing()
    test_placement_goes_away_with_its_pane()
    test_scrolled_away_placements_are_dropped()
    sys.exit(report())
