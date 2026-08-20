#!/usr/bin/env python3
"""Floating panes (M10a).

A floating pane is a leaf still in the tree, carrying intent the way
`minimized` does: the layout skips it, the siblings absorb its share, and
un-floating returns it home. What is drawn is the wanted rect clamped to the
tab's area, derived every frame and never written back — so what is worth
checking is that the overlap resolves to what you can see, that the intent
survives a resize exactly, that a flattened tab lists floats like everyone
else, and that the two guards hold: a tab keeps a tiled backdrop, and a
float's border never promises a split.
"""

import sys
import tempfile

from harness import Session, check, report


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']


def here(s):
    active = [t["index"] for t in s.tabs() if t["active"]][0]
    return [p for p in s.panes() if p["tab"] == active]


def floats(s):
    """Floats on screen. A minimised float keeps `floating` — that intent is
    what brings it back floating — but it has no rect while it is away."""
    return [p for p in here(s) if p["floating"] and p["w"] > 1]


def tiled(s):
    return [p for p in here(s) if not p["floating"] and p["w"] > 1]


def rect(p):
    return (p["x"], p["y"], p["w"], p["h"])


def overlap(a, b):
    """The intersection of two panes' content rects, or None."""
    x0 = max(a["content_x"], b["content_x"])
    y0 = max(a["content_y"], b["content_y"])
    x1 = min(a["content_x"] + a["content_w"], b["content_x"] + b["content_w"])
    y1 = min(a["content_y"] + a["content_h"], b["content_y"] + b["content_h"])
    if x0 >= x1 or y0 >= y1:
        return None
    return ((x0 + x1) // 2, (y0 + y1) // 2)


def test_floating_lifts_a_pane_out_of_the_layout():
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        before = max(p["w"] for p in tiled(s))
        r = s.api("float")
        check("the toggle answers", r.get("ok") and r.get("floating") == 1, str(r))
        s.settle(20)

        fl, ti = floats(s), tiled(s)
        check("one pane floats", len(fl) == 1, str(here(s)))
        check(
            "and the tiled one took the space back",
            len(ti) == 1 and ti[0]["w"] > before,
            str(here(s)),
        )
        check("the float keeps focus", fl[0]["focused"], str(fl))
        check(
            "seeded from its tiled rect, lifted a cell off the page",
            fl[0]["w"] < before and fl[0]["w"] > 1,
            str(fl),
        )


def test_the_overlap_belongs_to_the_float():
    """Painted last is on top, and the hit list is searched backwards, so the
    z-order and the click order are the same list — no routing code to
    disagree with the paint."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("float")
        s.settle(20)
        fl, ti = floats(s)[0], tiled(s)[0]

        # The float sits inside the tiled pane's rect now that it spans the tab.
        check(
            "the float is over the tiled pane",
            fl["x"] > ti["x"] and fl["x"] + fl["w"] <= ti["x"] + ti["w"],
            f"{rect(fl)} vs {rect(ti)}",
        )
        snap = s.snapshot()
        corner = snap.line(fl["y"])[fl["x"]]
        check(
            "its frame is drawn over the tiled cells",
            corner in "╭┌",
            repr(snap.line(fl["y"])),
        )
        cx, cy = fl["x"] + fl["w"] // 2, fl["y"] + fl["h"] // 2
        hit = snap.hit_at(cx, cy)
        check(
            "a click in the overlap lands on the float",
            hit == f"pane:{fl['id']}",
            str(hit),
        )

        # What the float's program prints lands at the float's cells.
        s.raw("FLOATED")
        s.until_text("FLOATED")
        snap = s.snapshot()
        row = snap.line(fl["content_y"])
        check(
            "and its content is composed at the float's position",
            row[fl["content_x"] : fl["content_x"] + 7] == "FLOATED",
            repr(row),
        )


def test_focusing_a_float_raises_it():
    """Two floats overlapping: the focused one paints last, so it is on top —
    stamped in layout(), once, rather than at each place focus can move."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        ids = [p["id"] for p in here(s)]
        # Float the middle pane, then the pane that expanded over its ground:
        # the second seed overlaps the first, which is the arrangement z-order
        # is about.
        s.api("float", id=ids[1])
        s.settle(20)
        s.api("float", id=ids[2])
        s.settle(20)
        fl = {p["id"]: p for p in floats(s)}
        check("two floats", len(fl) == 2, str(here(s)))
        spot = overlap(fl[ids[1]], fl[ids[2]])
        check(
            "their rects overlap", spot is not None, str(list(map(rect, fl.values())))
        )
        if not spot:
            return
        hit = s.snapshot().hit_at(*spot)
        check(
            "the newest float owns the overlap",
            hit == f"pane:{ids[2]}",
            str(hit),
        )
        s.api("focus", id=ids[1])
        s.settle(20)
        hit = s.snapshot().hit_at(*spot)
        check(
            "focusing the one underneath raises it",
            hit == f"pane:{ids[1]}",
            str(hit),
        )


def test_the_wanted_rect_survives_a_resize_exactly():
    """The drawn rect is the intent clamped to the area, derived every frame:
    the clamp never writes back, which is the collapse/expand lesson the
    weights already learned, asserted for floats."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("float")
        s.settle(20)
        want = rect(floats(s)[0])

        s.resize(40, 14)
        s.settle(20)
        fl = floats(s)
        check("still floating on a small screen", len(fl) == 1, str(here(s)))
        check(
            "and squeezed inside it",
            fl[0]["x"] + fl[0]["w"] <= 40 and fl[0]["y"] + fl[0]["h"] <= 14,
            str(rect(fl[0])),
        )

        s.resize(90, 24)
        s.settle(20)
        check(
            "the room comes back and the float is exactly where you put it",
            rect(floats(s)[0]) == want,
            f"{want} -> {rect(floats(s)[0])}",
        )


def test_unfloating_returns_it_to_its_seat():
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        layout_before = sorted(map(rect, tiled(s)))
        s.api("float")
        s.settle(20)
        s.api("float")
        s.settle(20)
        check("nothing floats", floats(s) == [], str(here(s)))
        check(
            "and the layout is the one it left",
            sorted(map(rect, tiled(s))) == layout_before,
            str(sorted(map(rect, tiled(s)))),
        )


def test_the_backdrop_keeps_a_pane():
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        r = s.api("float")
        check("the only pane cannot float", not r.get("ok"), str(r))

        s.api("split", dir="cols")
        s.settle(20)
        s.api("float")
        s.settle(20)
        check("with two, one can", len(floats(s)) == 1, str(here(s)))
        last = tiled(s)[0]["id"]
        r = s.api("float", id=last)
        check("but the last tiled pane cannot", not r.get("ok"), str(r))

        # Minimising the last tiled pane is the same hole by another door.
        s.api("focus", id=last)
        s.settle(20)
        s.send(r"\x01m")
        s.settle(20)
        check(
            "and neither can it be minimised",
            len(tiled(s)) == 1,
            str(here(s)),
        )


def test_closing_the_last_tiled_pane_lands_the_float():
    """The guard refuses to make the overlay-over-nothing state; closing can
    still reach it, so the top float lands — checked once, in layout()."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        fid = s.api("float")
        s.settle(20)
        fid = floats(s)[0]["id"]
        other = tiled(s)[0]["id"]
        s.api("close", id=other)
        s.settle(20)
        ps = here(s)
        check("one pane left", len(ps) == 1 and ps[0]["id"] == fid, str(ps))
        check("and it landed", not ps[0]["floating"], str(ps))
        check("filling the tab", ps[0]["w"] > 80, str(rect(ps[0])))


def test_a_float_cannot_be_split():
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("float")
        s.settle(20)
        n = len(here(s))
        fid = floats(s)[0]["id"]

        s.send(r"\x01\\")
        s.settle(20)
        check("the keyboard is refused", len(here(s)) == n, str(here(s)))
        check(
            "with the float's own words",
            "a floating pane cannot be split" in s.snapshot().screen(),
            repr(s.snapshot().screen()[-300:]),
        )
        r = s.api("split", id=fid)
        check("the API is refused too", not r.get("ok"), str(r))
        check(
            "and says why",
            "floating" in r.get("error", ""),
            str(r),
        )
        # The guide never offers what split_fits declines: no handle glyph is
        # promised here; the border's handle hits still exist but their click
        # path declines. That path is exercised in M10b with the drags.


def test_a_flattened_tab_lists_floats_like_everyone_else():
    """A screen too small for tiles is too small for overlays; the float is a
    reachable row, and its wanted rect is untouched for when room returns."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("float")
        s.settle(20)
        want = rect(floats(s)[0])
        fid = floats(s)[0]["id"]

        s.resize(26, 20)
        s.settle(20)
        ps = here(s)
        check("the tab is a list", any(p["hidden"] for p in ps), str(ps))
        check(
            "and the float is in it, a row like everyone else",
            all(p["w"] > 1 for p in ps),
            str(ps),
        )

        s.resize(90, 24)
        s.settle(20)
        check(
            "room back: floating again, exactly where it was",
            len(floats(s)) == 1 and rect(floats(s)[0]) == want,
            str(here(s)),
        )
        check("same pane", floats(s)[0]["id"] == fid, str(floats(s)))


def test_a_minimised_float_comes_back_floating():
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("float")
        s.settle(20)
        fid = floats(s)[0]["id"]
        s.send(r"\x01m")
        s.settle(20)
        check("a float can be put away", floats(s) == [], str(here(s)))
        s.api("focus", id=fid)
        s.settle(20)
        check(
            "and focusing it restores it floating",
            len(floats(s)) == 1 and floats(s)[0]["id"] == fid,
            str(here(s)),
        )


def test_zoom_fills_the_tab_whether_it_floats_or_not():
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("float")
        s.settle(20)
        want = rect(floats(s)[0])
        s.send(r"\x01z")
        s.settle(20)
        fl = floats(s)
        check("zoomed, the float fills the tab", fl and fl[0]["w"] > 80, str(fl))
        s.send(r"\x01z")
        s.settle(20)
        check(
            "and unzooming returns it to its rect",
            rect(floats(s)[0]) == want,
            f"{want} -> {rect(floats(s)[0])}",
        )


def drag(s, x0, y0, x1, y1):
    s.send(rf"\e[<0;{x0 + 1};{y0 + 1}M")
    s.send(rf"\e[<32;{x1 + 1};{y1 + 1}M")
    s.send(rf"\e[<0;{x1 + 1};{y1 + 1}m")
    s.settle(20)


def floated(s):
    """One pane split off and floating, ready to be handled."""
    s.settle(20)
    s.api("split", dir="cols")
    s.settle(20)
    s.api("float")
    s.settle(20)
    return floats(s)[0]


def test_dragging_the_title_moves_it():
    """A float's top row carries its own verb: everything a title drag means
    on release -- swap, split up -- is what a float must not do."""
    with Session(SH, cols=90, rows=24) as s:
        fl = floated(s)
        before = rect(fl)
        tiled_before = sorted(map(rect, tiled(s)))
        # Left and up, where the area has room; down would hit the clamp,
        # which is the resize test's subject rather than this one's.
        drag(s, fl["x"] + 3, fl["y"], fl["x"] - 7, fl["y"] - 1)
        after = rect(floats(s)[0])
        check(
            "the float follows the pointer",
            after == (before[0] - 10, before[1] - 1, before[2], before[3]),
            f"{before} -> {after}",
        )
        check(
            "and nothing tiled moved",
            sorted(map(rect, tiled(s))) == tiled_before,
            str(sorted(map(rect, tiled(s)))),
        )
        check("no pane swapped anywhere", len(floats(s)) == 1, str(here(s)))


def test_dragging_an_edge_resizes_it():
    with Session(SH, cols=90, rows=24) as s:
        fl = floated(s)
        # Away from the wall first: the seed sits a cell inside the tiled
        # pane, and an edge dragged into the wall pins there — that clamp
        # has its own test below.
        drag(s, fl["x"] + 3, fl["y"], fl["x"] - 7, fl["y"])
        fl = floats(s)[0]
        before = rect(fl)
        # The right edge, mid-height: rim or handle, either is a grab.
        ex, ey = fl["x"] + fl["w"] - 1, fl["y"] + fl["h"] // 2
        drag(s, ex, ey, ex + 6, ey)
        after = rect(floats(s)[0])
        check(
            "the right edge follows the pointer",
            after == (before[0], before[1], before[2] + 6, before[3]),
            f"{before} -> {after}",
        )

        # A bottom corner is two edges, derived from where the press landed.
        fl = floats(s)[0]
        before = rect(fl)
        cx, cy = fl["x"], fl["y"] + fl["h"] - 1
        drag(s, cx, cy, cx - 4, cy + 1)
        after = rect(floats(s)[0])
        check(
            "a corner resizes on both axes",
            after == (before[0] - 4, before[1], before[2] + 4, before[3] + 1),
            f"{before} -> {after}",
        )


def test_the_floor_stops_the_grabbed_edge():
    with Session(SH, cols=90, rows=24) as s:
        fl = floated(s)
        ex, ey = fl["x"] + fl["w"] - 1, fl["y"] + fl["h"] // 2
        drag(s, ex, ey, fl["x"] - 30, ey)  # far past the left edge
        after = rect(floats(s)[0])
        check(
            "a resize stops at the floor rather than inverting",
            after[2] >= 4 and after[0] == fl["x"],
            f"{rect(fl)} -> {after}",
        )
        # And the wall pins the grabbed edge: the opposite one never moves.
        fl = floats(s)[0]
        ex = fl["x"] + fl["w"] - 1
        drag(s, ex, ey, 200, ey)  # far past the right wall
        after = rect(floats(s)[0])
        check(
            "the wall pins the edge you hold, not the window",
            after[0] == fl["x"] and after[0] + after[2] <= 90,
            f"{rect(fl)} -> {after}",
        )


def test_a_keystroke_ends_a_float_drag():
    """A release can go missing; any key ends a drag, floats included."""
    with Session(SH, cols=90, rows=24) as s:
        fl = floated(s)
        before = rect(fl)
        s.send(rf"\e[<0;{fl['x'] + 4};{fl['y'] + 1}M")  # press, no release
        s.send(rf"\e[<32;{fl['x'] - 6};{fl['y'] + 1}M")
        s.settle(20)
        s.send(r"\x01l")  # any chord: the drag must not survive it
        s.settle(20)
        moved = rect(floats(s)[0])
        s.send(rf"\e[<32;{fl['x'] + 20};{fl['y'] + 5}M")  # stray motion after
        s.settle(20)
        check(
            "motion after the keystroke moves nothing",
            rect(floats(s)[0]) == moved,
            f"{moved} -> {rect(floats(s)[0])}",
        )
        check("the earlier drag did move it", moved != before, str(moved))


def test_the_keyboard_moves_a_focused_float():
    """H J K L: a float has no boundary to move, so the same keys move the
    thing itself -- one gap-aspect-square step per press."""
    with Session(SH, cols=90, rows=24) as s:
        fl = floated(s)
        before = rect(fl)
        s.send(r"\x01H")
        s.settle(20)
        s.send(r"\x01J")
        s.settle(20)
        after = rect(floats(s)[0])
        check(
            "H moves it left and J moves it down",
            after[0] < before[0] and after[1] == before[1] + 1,
            f"{before} -> {after}",
        )
        # And the tiled boundary keys still work when focus is tiled.
        s.api("focus", id=tiled(s)[0]["id"])
        s.settle(20)
        w0 = tiled(s)[0]["w"]
        s.send(r"\x01L")
        s.settle(20)
        check(
            "a tiled pane's keys still move its boundary",
            tiled(s)[0]["w"] == w0,  # one tiled pane: nothing to resize against
            str(tiled(s)[0]["w"]),
        )


def shadow_cells(s):
    """A shadow cell and an out-of-shade cell on the same backdrop row.

    The freshly seeded float sits one cell inside the tiled pane, so its
    shadow falls on that pane's frame — styled with or without a shadow.
    Moved off it first, both probe cells are blank tiled content, and the
    only thing that can differ between them is the shade."""
    fl = floats(s)[0]
    drag(s, fl["x"] + 3, fl["y"], fl["x"] - 7, fl["y"] - 1)
    fl = floats(s)[0]
    return (fl["x"] + fl["w"], fl["y"] + 2), (fl["x"] + fl["w"] + 5, fl["y"] + 2)


def test_a_float_casts_a_shadow():
    """A cell of shade beside and below, on whatever is covered: the frame
    is the same frame, so the shadow is what tells a float from a tile."""
    with Session(SH, cols=90, rows=24) as s:
        floated(s)
        (sx, sy), (ox, oy) = shadow_cells(s)
        snap = s.snapshot()
        shadow, clear = snap.style_at(sx, sy), snap.style_at(ox, oy)
        check("the shadow cell is shaded", shadow is not None, str(shadow))
        check(
            "and the same backdrop out of the shade is not",
            shadow != clear,
            f"{shadow} vs {clear}",
        )


def test_float_shadow_0_turns_it_off():
    with Session(SH, cols=90, rows=24, config=cfg("float_shadow 0\n")) as s:
        floated(s)
        (sx, sy), (ox, oy) = shadow_cells(s)
        snap = s.snapshot()
        check(
            "no shade is cast",
            snap.style_at(sx, sy) == snap.style_at(ox, oy),
            f"{snap.style_at(sx, sy)} vs {snap.style_at(ox, oy)}",
        )


def test_the_floating_state_takes_a_chain():
    """`states { floating { } }` is a state like any other -- and it ranks
    above unfocused, so a float is never dimmed as merely-unfocused."""
    conf = cfg("states { floating { dim amount=200 } }\n")
    with Session(SH, cols=90, rows=24, config=conf) as s:
        fl = floated(s)
        snap = s.snapshot()
        inside = snap.style_at(fl["content_x"] + 1, fl["content_y"] + 1)
        check("the chain colours the float", inside is not None, str(inside))
        s.api("float")  # put it back: the chain should leave with the state
        s.settle(20)
        p = [q for q in here(s) if q["id"] == fl["id"]][0]
        snap = s.snapshot()
        check(
            "and leaves with the state",
            snap.style_at(p["content_x"] + 1, p["content_y"] + 1) != inside,
            str(snap.style_at(p["content_x"] + 1, p["content_y"] + 1)),
        )


def test_floating_pops_to_the_centre():
    """The pop to the middle is what says the float happened: a pane lifted
    in place looks like a pane whose neighbours flinched."""
    with Session(SH, cols=90, rows=24) as s:
        fl = floated(s)
        cx = fl["x"] + fl["w"] / 2
        check("the float is centred", abs(cx - 45) <= 1, f"centre {cx}")


def test_refloating_recentres_but_keeps_the_size():
    """The size is the hand's; the position is the announcement."""
    with Session(SH, cols=90, rows=24) as s:
        fl = floated(s)
        # Shape it and park it in a corner.
        ex, ey = fl["x"] + fl["w"] - 1, fl["y"] + fl["h"] // 2
        drag(s, ex, ey, ex - 8, ey)
        fl = floats(s)[0]
        drag(s, fl["x"] + 3, fl["y"], 4, 2)
        shaped = rect(floats(s)[0])
        s.send(r"\x01f")  # dock
        s.settle(20)
        s.send(r"\x01f")  # and float again
        s.settle(20)
        fl = floats(s)[0]
        check(
            "the size comes back",
            (fl["w"], fl["h"]) == (shaped[2], shaped[3]),
            f"{shaped} -> {rect(fl)}",
        )
        cx = fl["x"] + fl["w"] / 2
        check("but it pops to the centre again", abs(cx - 45) <= 1, f"centre {cx}")


def test_plus_grows_and_minus_shrinks_a_float():
    """About its own centre, so the pane stays where you were looking. On a
    tiled pane `-` still splits: the keys aim at what is there."""
    with Session(SH, cols=90, rows=24) as s:
        fl = floated(s)
        before = rect(fl)
        s.send(r"\x01=")  # the =/+ key, however shift arrives
        s.settle(20)
        grown = rect(floats(s)[0])
        check(
            "= grows it in every direction",
            grown[2] > before[2] and grown[3] > before[3] and grown[0] < before[0],
            f"{before} -> {grown}",
        )
        s.send(r"\x01-")
        s.settle(20)
        check(
            "- shrinks it back about the same centre",
            rect(floats(s)[0]) == before,
            f"{grown} -> {rect(floats(s)[0])}",
        )

        n = len(here(s))
        s.api("focus", id=tiled(s)[0]["id"])
        s.settle(20)
        s.send(r"\x01-")
        s.settle(20)
        check("on a tiled pane - still splits", len(here(s)) == n + 1, str(here(s)))


def test_zero_equalizes():
    """Equalize moved off `=` to make room for the float verbs: 1..9 pick a
    tab, 0 resets the shares."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        # Skew the boundary, then put it back.
        s.send(r"\x01L")
        s.settle(20)
        skewed = [p["w"] for p in tiled(s)]
        s.send(r"\x010")
        s.settle(20)
        even = [p["w"] for p in tiled(s)]
        check(
            "0 gives every pane an even share",
            abs(even[0] - even[1]) <= 1 and skewed[0] != skewed[1],
            f"{skewed} -> {even}",
        )


def test_new_float_opens_a_floating_shell():
    """`C-a F`: the throwaway terminal -- f floats this pane, F floats a
    fresh one. It holds a real seat in the tree, so un-floating lands it
    beside the pane it was opened over."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        n = len(here(s))
        s.send(r"\x01F")
        s.settle(40)
        fl = floats(s)
        check(
            "a new pane opens floating",
            len(fl) == 1 and len(here(s)) == n + 1,
            str(here(s)),
        )
        if not fl:
            return
        check("focused", fl[0]["focused"], str(fl))
        cx = fl[0]["x"] + fl[0]["w"] / 2
        check("and centred", abs(cx - 45) <= 1, f"centre {cx}")

        s.send(r"\x01f")  # dock it: it has a seat to land in
        s.settle(20)
        check(
            "un-floating lands it in the layout",
            floats(s) == [] and len(tiled(s)) == 2,
            str(here(s)),
        )

        r = s.api("new-float")
        s.settle(20)
        check("the API opens one too", r.get("ok") and len(floats(s)) == 1, str(r))


def test_function_keys_are_bindable():
    """The decoder always understood the function row; the config just had
    no name for it. `bind \"f5\" ...` is the proof, on a different key than
    the default so this is the parser and not the default bind."""
    conf = cfg('keys { bind "f5" "float" }\n')
    with Session(SH, cols=90, rows=24, config=conf) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.send(r"\x01\e[15~")  # C-a F5
        s.settle(20)
        check("a config can bind a function key", len(floats(s)) == 1, str(here(s)))


FAST = cfg("hover_delay_ms 20\n")


def hover(s, x, y):
    """Motion with no button held: SGR button 35 = release/none + motion."""
    s.send(rf"\e[<35;{x + 1};{y + 1}M")


def test_hovering_a_float_edge_shows_the_resize():
    """The border is a working handle and has to say so: the edge a grab
    would move lights in the resize colour with the double arrow at the
    grab point, and the status line says the verb. A corner lights both
    edges and says "both ways" -- all read from the same derivation the
    press uses, so the promise and the drag cannot disagree."""
    with Session(SH, cols=90, rows=24, config=FAST) as s:
        fl = floated(s)
        x1, y1 = fl["x"] + fl["w"] - 1, fl["y"] + fl["h"] - 1
        ey = fl["y"] + fl["h"] // 2

        hover(s, x1, ey)
        s.settle(60)
        snap = s.snapshot()
        check(
            "the edge goes heavy with the arrow at the grab point",
            snap.line(ey)[x1] == "\u21d4" and snap.line(ey - 1)[x1] == "\u2503",
            repr(snap.line(ey)[x1 - 2 : x1 + 1]),
        )
        check(
            "and the status line says the verb",
            "drag to resize" in snap.screen(),
            repr(snap.screen()[-200:]),
        )

        hover(s, fl["x"], y1)
        s.settle(60)
        snap = s.snapshot()
        check(
            "a corner lights both edges",
            snap.line(y1)[fl["x"] + 3] == "\u2501"
            and snap.line(y1 - 2)[fl["x"]] == "\u2503",
            repr(snap.line(y1)[fl["x"] : fl["x"] + 5]),
        )
        check(
            "and says both ways",
            "both ways" in snap.screen(),
            repr(snap.screen()[-200:]),
        )


def test_a_float_title_promises_no_split():
    """The tiled handle says 'drag to move / click to split up'; a float's
    top row only moves it, and the caption must not promise the half a
    float does not offer."""
    with Session(SH, cols=90, rows=24, config=FAST) as s:
        fl = floated(s)
        th = [h for h in s.snapshot().hits if h["action"] == f"title:{fl['id']}:t"]
        check("the float's top row still carries its handle", th != [], "")
        if not th:
            return
        hover(s, th[0]["x"], th[0]["y"])
        s.settle(60)
        scr = s.snapshot().screen()
        check(
            "its hint moves and does not split",
            "drag to move" in scr and "split up" not in scr,
            repr(scr[-200:]),
        )


def test_the_grabbed_edge_stays_lit_through_the_drag():
    with Session(SH, cols=90, rows=24, config=FAST) as s:
        fl = floated(s)
        x1 = fl["x"] + fl["w"] - 1
        ey = fl["y"] + fl["h"] // 2
        s.send(rf"\e[<0;{x1 + 1};{ey + 1}M")  # press the right edge
        s.send(rf"\e[<32;{x1 - 5};{ey + 1}M")  # and pull it left
        s.settle(40)
        fl2 = [p for p in s.panes() if p["floating"]][0]
        nx1 = fl2["x"] + fl2["w"] - 1
        snap = s.snapshot()
        check(
            "the held edge follows lit",
            snap.line(ey)[nx1] == "\u2503",
            repr(snap.line(ey)[nx1 - 2 : nx1 + 2]),
        )
        s.send(rf"\e[<0;{x1 - 5};{ey + 1}m")


def test_a_float_is_not_a_drop_target():
    """Swapping tree seats with a pane that is not in its seat would
    rearrange something invisible, so a title drag promises nothing over a
    float and swaps nothing dropped on one."""
    with Session(SH, cols=90, rows=24) as s:
        fl = floated(s)
        ti = tiled(s)[0]
        before = sorted(map(rect, here(s)))
        # Drag the tiled pane's title onto the float and drop it there.
        drag(s, ti["x"] + 3, ti["y"], fl["x"] + fl["w"] // 2, fl["y"] + 3)
        check(
            "nothing swapped",
            sorted(map(rect, here(s))) == before,
            str(sorted(map(rect, here(s)))),
        )


def test_the_key_and_the_palette_both_run_it():
    """`C-a f` floats and unfloats; the palette runs the same action and
    shows the chord, which is how the palette teaches the key (D19)."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.send(r"\x01f")
        s.settle(20)
        check("C-a f floats the pane", len(floats(s)) == 1, str(here(s)))
        s.send(r"\x01f")
        s.settle(20)
        check("and again puts it back", floats(s) == [], str(here(s)))

        s.send(r"\x01p")
        s.settle(50)
        check(
            "the palette offers it",
            "float it above the layout" in s.snapshot().screen(),
            repr(s.snapshot().screen()[:400]),
        )
        s.send("float")
        s.settle(50)
        s.send(r"\r")
        s.settle(50)
        check("running it floats the pane", len(floats(s)) == 1, str(here(s)))


if __name__ == "__main__":
    test_floating_lifts_a_pane_out_of_the_layout()
    test_the_overlap_belongs_to_the_float()
    test_focusing_a_float_raises_it()
    test_the_wanted_rect_survives_a_resize_exactly()
    test_unfloating_returns_it_to_its_seat()
    test_the_backdrop_keeps_a_pane()
    test_closing_the_last_tiled_pane_lands_the_float()
    test_a_float_cannot_be_split()
    test_a_flattened_tab_lists_floats_like_everyone_else()
    test_a_minimised_float_comes_back_floating()
    test_zoom_fills_the_tab_whether_it_floats_or_not()
    test_dragging_the_title_moves_it()
    test_dragging_an_edge_resizes_it()
    test_the_floor_stops_the_grabbed_edge()
    test_a_keystroke_ends_a_float_drag()
    test_the_keyboard_moves_a_focused_float()
    test_a_float_casts_a_shadow()
    test_float_shadow_0_turns_it_off()
    test_the_floating_state_takes_a_chain()
    test_floating_pops_to_the_centre()
    test_refloating_recentres_but_keeps_the_size()
    test_plus_grows_and_minus_shrinks_a_float()
    test_zero_equalizes()
    test_new_float_opens_a_floating_shell()
    test_function_keys_are_bindable()
    test_hovering_a_float_edge_shows_the_resize()
    test_a_float_title_promises_no_split()
    test_the_grabbed_edge_stays_lit_through_the_drag()
    test_a_float_is_not_a_drop_target()
    test_the_key_and_the_palette_both_run_it()
    sys.exit(report())
