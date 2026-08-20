#!/usr/bin/env python3
"""The drop grammar: where on the target you let go is what the drop means.

While a pane is carried, the pane under the pointer subdivides into a centre
-- the swap it always was, keeping the biggest target -- and four edge bands
meaning "insert me beside this pane, on this side", which is what turns
dragging into re-layout. Zones materialise only under the pointer, are
registered in the hit list by the paint that shows them, and a side whose
insert would not fit never registers at all -- so the promise on screen and
the drop on release are the same rects.

A note on flow: the drop resolves against the *painted* frame's hits, so
routing can never consult geometry the user never saw. The live server
composes every frame; this driver composes on snapshot/panes, so each drag
takes a snapshot between motion and release, standing in for the frames a
real session paints continuously.
"""

import sys

from harness import Session, check, report

SH = ["/bin/sh", "-c", 'printf "\\033]2;p\\007"; stty raw -echo; cat']


def by_id(s):
    return {p["id"]: p for p in s.panes()}


def rect(p):
    return (p["x"], p["y"], p["w"], p["h"])


def drag_to(s, src, x, y, release=True):
    """Press src's title, move to (x, y), compose, optionally release."""
    s.send(rf"\e[<0;{src['x'] + 4};{src['y'] + 1}M")
    s.send(rf"\e[<32;{x + 1};{y + 1}M")
    snap = s.snapshot()  # the frame the user would have seen
    if release:
        s.send(rf"\e[<0;{x + 1};{y + 1}m")
        s.settle(30)
    return snap


def test_a_band_drop_inserts_beside_the_target():
    """Three panes, so an insert is geometrically distinct from a swap."""
    with Session(SH, cols=120, rows=30) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        a, b, c = sorted(by_id(s))
        ps = by_id(s)

        # Drag A into C's bottom band: A lands under C, B untouched.
        snap = drag_to(
            s, ps[a], ps[c]["x"] + ps[c]["w"] // 2, ps[c]["y"] + ps[c]["h"] - 2
        )
        ps = by_id(s)
        check(
            "the pane lands below the target",
            ps[a]["y"] > ps[c]["y"] and ps[a]["x"] == ps[c]["x"],
            f"A {rect(ps[a])} C {rect(ps[c])}",
        )
        check("and keeps the keyboard", ps[a]["focused"], str(ps[a]))
        check(
            "the bystander is untouched horizontally",
            ps[b]["y"] == 2,
            str(rect(ps[b])),
        )

        # And back out: drag A onto B's left band. A column split this time.
        ps = by_id(s)
        drag_to(s, ps[a], ps[b]["x"] + 1, ps[b]["y"] + ps[b]["h"] // 2)
        ps = by_id(s)
        check(
            "a left-band drop lands it beside, before",
            ps[a]["x"] < ps[b]["x"] and ps[a]["y"] == ps[b]["y"],
            f"A {rect(ps[a])} B {rect(ps[b])}",
        )


def test_the_centre_is_still_the_swap():
    with Session(SH, cols=120, rows=30) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("split", dir="rows")
        s.settle(20)
        a, b, c = sorted(by_id(s))
        ps = by_id(s)
        ra, rc = rect(ps[a]), rect(ps[c])
        drag_to(s, ps[a], ps[c]["x"] + ps[c]["w"] // 2, ps[c]["y"] + ps[c]["h"] // 2)
        ps = by_id(s)
        check(
            "dropping on the centre trades places",
            rect(ps[a]) == rc and rect(ps[c]) == ra,
            f"A {rect(ps[a])} C {rect(ps[c])}",
        )


def test_zones_materialise_only_under_the_pointer():
    with Session(SH, cols=120, rows=30) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        a, b, c = sorted(by_id(s))
        ps = by_id(s)
        snap = drag_to(
            s,
            ps[a],
            ps[b]["x"] + ps[b]["w"] // 2,
            ps[b]["y"] + ps[b]["h"] - 2,
            release=False,
        )
        zones = {h["action"] for h in snap.hits if h["action"].startswith("drop:")}
        check(
            "the hovered pane offers its zones",
            zones and all(z.startswith(f"drop:{b}:") for z in zones),
            str(zones),
        )
        check(
            "and no other pane offers any",
            not any(z.startswith(f"drop:{c}:") for z in zones),
            str(zones),
        )
        # The hovered band fills the half the drop would hand over.
        B = ps[b]
        low = snap.style_at(B["x"] + 4, B["y"] + B["h"] - 3)
        high = snap.style_at(B["x"] + 4, B["y"] + 2)
        check("the claimed half is filled", low != high, f"{low} vs {high}")
        s.send(r"\e")  # any key ends the drag


def test_a_zone_that_cannot_fit_is_not_offered():
    """The refusal is the absence of the band, the same floor a border
    click's split honours -- so the preview cannot promise what the drop
    cannot deliver. The dragged pane must not be the target's sibling along
    the refused axis, because a drop beside a sibling is a reorder and a
    reorder always fits."""
    with Session(SH, cols=60, rows=44) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("split", dir="rows")
        s.settle(20)
        a, b, c = sorted(by_id(s))
        ps = by_id(s)
        # C sits under B in a rows split; A is across in the root columns
        # split, so it is nobody's sibling along cols. C is ~28 cols: laid
        # out fine, too narrow to host another column, so no upright bands;
        # its rows still fit.
        snap = drag_to(
            s,
            ps[a],
            ps[c]["x"] + 1,
            ps[c]["y"] + ps[c]["h"] // 2,
            release=False,
        )
        zones = {h["action"] for h in snap.hits if h["action"].startswith("drop:")}
        check(
            "no column bands on a pane too narrow to split",
            not any(z.endswith(":l") or z.endswith(":r") for z in zones),
            str(zones),
        )
        check(
            "while the row bands still offer",
            any(z.endswith(":t") for z in zones),
            str(zones),
        )
        s.send(r"\e")


def test_a_reorder_among_siblings_is_always_offered():
    """Dragging a pane onto its neighbour's far band rearranges the row: the
    pane leaves the split it rejoins, so the child count never changes and
    there is nothing to fit — refusing it was refusing the most ordinary
    drag there is."""
    with Session(SH, cols=100, rows=28) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        a, b = sorted(by_id(s))
        ps = by_id(s)
        # ~47 cols each: far too narrow for a third column, but these two
        # are siblings along cols, so the bands offer the reorder.
        snap = drag_to(
            s,
            ps[a],
            ps[b]["x"] + ps[b]["w"] - 2,
            ps[b]["y"] + ps[b]["h"] // 2,
            release=False,
        )
        zones = {h["action"] for h in snap.hits if h["action"].startswith("drop:")}
        check(
            "the sibling's column bands are offered",
            any(z.endswith(":r") for z in zones),
            str(zones),
        )
        s.send(
            rf"\e[<0;{ps[b]['x'] + ps[b]['w'] - 1};{ps[b]['y'] + ps[b]['h'] // 2 + 1}m"
        )
        s.settle(30)
        ps = by_id(s)
        check(
            "and the drop rearranges the row",
            ps[a]["x"] > ps[b]["x"] and ps[a]["y"] == ps[b]["y"],
            f"A {rect(ps[a])} B {rect(ps[b])}",
        )


def test_a_float_offers_no_zones():
    with Session(SH, cols=120, rows=30) as s:
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        s.api("split", dir="cols")
        s.settle(20)
        ids = sorted(by_id(s))
        s.api("float", id=ids[2])
        s.settle(20)
        ps = by_id(s)
        fl = ps[ids[2]]
        snap = drag_to(
            s,
            ps[ids[0]],
            fl["x"] + fl["w"] // 2,
            fl["y"] + fl["h"] - 2,
            release=False,
        )
        zones = {h["action"] for h in snap.hits if h["action"].startswith("drop:")}
        check("a float subdivides for nobody", zones == set(), str(zones))
        s.send(r"\e")


if __name__ == "__main__":
    test_a_band_drop_inserts_beside_the_target()
    test_the_centre_is_still_the_swap()
    test_zones_materialise_only_under_the_pointer()
    test_a_zone_that_cannot_fit_is_not_offered()
    test_a_reorder_among_siblings_is_always_offered()
    test_a_float_offers_no_zones()
    sys.exit(report())
