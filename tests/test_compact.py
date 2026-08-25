#!/usr/bin/env python3
"""Compact mode: shared 1-cell borders instead of gaps — and the border-drag
resize that `gap 0` needs either way.

Panes pack flush against divider lines that meet in real junctions, one frame
rings the tab, and a pane's title rides the line above it. The dividers carry
the same resize drag the gaps did; interior edges give up click-to-split; the
outer frame keeps it. Floats, zoom and the flatten keep the classic frame.
"""

import os
import sys
import tempfile
import time

from harness import Session, check, report


def _cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def _lay(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".layout", delete=False)
    f.write(text)
    f.close()
    return f.name


GAP0 = _cfg("gap 0\npadding 0\n")
COMPACT = _cfg("compact true\nhover_delay_ms 10\n")
GRID = _lay(
    'layout { tab { pane split="rows" { '
    'pane split="cols" { pane; pane }; '
    'pane split="cols" { pane; pane } } } }\n'
)

SH = ["/bin/sh", "-c", 'printf "\\033]2;pane\\007"; stty raw -echo; cat']


def drag(s, x0, y0, x1, y1):
    s.send(rf"\e[<0;{x0 + 1};{y0 + 1}M")
    s.send(rf"\e[<32;{x1 + 1};{y1 + 1}M")
    s.send(rf"\e[<0;{x1 + 1};{y1 + 1}m")
    s.settle(40)


# ---- gap 0: the border is all there is, so the border drags ---------------


def test_gap0_border_drag_moves_the_boundary():
    with Session(SH, cols=80, rows=20, config=GAP0) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left = min(s.panes(), key=lambda p: p["x"])
        bx = left["x"] + left["w"] - 1
        by = left["y"] + left["h"] // 2
        check(
            "flush panes leave no gap, so the cell is the border",
            s.snapshot().hit_at(bx, by) == f"border:{left['id']}:r",
            s.snapshot().hit_at(bx, by),
        )
        before = left["w"]
        drag(s, bx, by, bx + 6, by)
        after = min(s.panes(), key=lambda p: p["x"])["w"]
        check(
            "dragging the border moved the boundary",
            after == before + 6,
            f"{before} -> {after}",
        )


def test_gap0_rim_drags_but_never_splits():
    with Session(SH, cols=80, rows=20, config=GAP0) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        snap = s.snapshot()
        left = min(s.panes(), key=lambda p: p["x"])
        rim = snap.rim(left["id"], "r")
        check("the border still has a rim", rim is not None)
        s.click(*rim)
        s.settle()
        check(
            "a rim click still splits nothing", len(s.panes()) == 2, str(len(s.panes()))
        )
        before = left["w"]
        drag(s, rim[0], rim[1], rim[0] - 5, rim[1])
        after = min(s.panes(), key=lambda p: p["x"])["w"]
        check(
            "but a rim drag moves the boundary",
            after == before - 5,
            f"{before} -> {after}",
        )


def test_gap0_handle_click_still_splits():
    with Session(SH, cols=120, rows=20, config=GAP0) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        left = min(s.panes(), key=lambda p: p["x"])
        h = s.snapshot().handle(left["id"], "r")
        s.click(*h)
        s.settle()
        check(
            "a handle click is still the split it always was",
            len(s.panes()) == 3,
            str(len(s.panes())),
        )


# ---- compact: the look ----------------------------------------------------


def test_compact_boundary_costs_one_cell():
    with Session(SH, cols=64, rows=22, config=COMPACT, layout=GRID) as s:
        s.settle(60)
        ps = sorted(s.panes(), key=lambda p: (p["y"], p["x"]))
        tl, tr = ps[0], ps[1]
        check(
            "one cell between side-by-side contents",
            tr["content_x"] - (tl["content_x"] + tl["content_w"]) == 1,
            f"{tl} | {tr}",
        )
        bl = ps[2]
        check(
            "one cell between stacked contents",
            bl["content_y"] - (tl["content_y"] + tl["content_h"]) == 1,
            f"{tl} / {bl}",
        )
        check(
            "content starts one cell in from the screen edge",
            tl["content_x"] == 1 and tl["content_y"] == 2,
            str(tl),
        )


def test_compact_junctions_are_real_glyphs():
    with Session(SH, cols=64, rows=22, config=COMPACT, layout=GRID) as s:
        s.settle(60)
        snap = s.snapshot()
        scr = snap.screen()
        for g in "┬┴├┤┼":
            check(f"the lines meet in a {g}", g in scr)
        corner = [h for h in snap.hits if h["action"].startswith("corner:")]
        check("the crossing is a drag target", len(corner) == 1, str(corner))
        c = corner[0]
        check(
            "and the target sits on the ┼ itself",
            snap.line(c["y"])[c["x"]] == "┼",
            repr(snap.line(c["y"])),
        )


def test_compact_titles_ride_the_shared_line():
    with Session(SH, cols=64, rows=22, config=COMPACT, layout=GRID) as s:
        s.settle(60)
        snap = s.snapshot()
        ps = sorted(s.panes(), key=lambda p: (p["y"], p["x"]))
        bl = ps[2]  # bottom-left: its title line is the interior divider
        hit = [h for h in snap.hits if h["action"] == f"panetitle:{bl['id']}"]
        check(
            "an interior pane's name is on the divider above it",
            hit and hit[0]["y"] == bl["y"] - 1,
            str(hit),
        )
        check(
            "and the name is drawn there",
            "pane" in snap.line(bl["y"] - 1),
            repr(snap.line(bl["y"] - 1)),
        )


# ---- compact: the mouse ---------------------------------------------------


def test_compact_dividers_drag_like_gaps():
    with Session(SH, cols=64, rows=22, config=COMPACT, layout=GRID) as s:
        s.settle(60)
        snap = s.snapshot()
        ps = sorted(s.panes(), key=lambda p: (p["y"], p["x"]))
        tl = ps[0]
        dx = tl["x"] + tl["w"]  # the divider column
        dy = tl["y"] + 2
        check(
            "the divider is the boundary's own hit",
            (snap.hit_at(dx, dy) or "").startswith("edge:"),
            snap.hit_at(dx, dy),
        )
        before = tl["w"]
        drag(s, dx, dy, dx + 5, dy)
        after = sorted(s.panes(), key=lambda p: (p["y"], p["x"]))[0]["w"]
        check(
            "dragging it moves the boundary",
            after == before + 5,
            f"{before} -> {after}",
        )


def test_compact_interior_edges_resize_but_do_not_split():
    with Session(SH, cols=64, rows=22, config=COMPACT, layout=GRID) as s:
        s.settle(60)
        snap = s.snapshot()
        ps = sorted(s.panes(), key=lambda p: (p["y"], p["x"]))
        bl = ps[2]
        divy = bl["y"] - 1
        free = snap.hit_at(bl["x"], divy)
        check(
            "a divider's free cells stay the resize handle",
            (free or "").startswith("edge:"),
            free,
        )
        interior = [
            h
            for h in snap.hits
            if h["action"] in (f"border:{bl['id']}:t", f"brim:{bl['id']}:t")
        ]
        check("an interior edge offers no split", not interior, str(interior))


def test_compact_outer_frame_still_splits():
    with Session(SH, cols=100, rows=22, config=COMPACT) as s:
        s.settle()
        p = s.pane(0)
        snap = s.snapshot()
        h = snap.handle(p["id"], "l")
        check("the outer frame has a handle", h is not None)
        check("on the frame's own column", h[0] == p["x"] - 1, str(h))
        s.click(*h)
        s.settle()
        check("and clicking it splits", len(s.panes()) == 2, str(len(s.panes())))


def test_compact_hover_hint_spares_the_title():
    with Session(SH, cols=64, rows=22, config=COMPACT, layout=GRID) as s:
        s.settle(60)
        ps = sorted(s.panes(), key=lambda p: (p["y"], p["x"]))
        bl = ps[2]
        divy = bl["y"] - 1
        s.send(rf"\e[<35;{bl['x'] + 2};{divy + 1}M")
        time.sleep(0.05)
        s.settle(30)
        row = s.snapshot().line(divy)
        check("the armed divider goes dotted", "┈" in row, repr(row))
        check("without ruling through the pane's name", " pane " in row, repr(row))


# ---- compact: the frame's colour and everything classic -------------------


def test_compact_focus_ring_wears_the_frame_colour():
    with Session(SH, cols=64, rows=22, config=COMPACT, layout=GRID) as s:
        s.settle(60)
        snap = s.snapshot()
        f = s.focused()
        ring = snap.style_at(f["x"] - 1, f["y"] + 1)
        # another pane's stretch of the frame, for contrast
        others = [p for p in s.panes() if p["id"] != f["id"]]
        far = max(others, key=lambda p: abs(p["x"] - f["x"]) + abs(p["y"] - f["y"]))
        idle = snap.style_at(far["x"] + far["w"], far["y"] + 1)
        check("the focused ring is coloured", ring is not None, str(ring))
        check(
            "differently from an idle stretch of line",
            idle is None or (ring or {}).get("fg") != idle.get("fg"),
            f"{ring} vs {idle}",
        )


def test_compact_dead_pane_epitaph_overlays_the_last_row():
    cfg = _cfg('compact true\nkeep_dead "all"\n')
    with Session(["/bin/sh", "-c", "exit 3"], cols=60, rows=12, config=cfg) as s:
        snap = s.until(lambda sn: "re-run" in sn.screen())
        p = s.pane(0)
        row = snap.line(p["content_y"] + p["content_h"] - 1)
        check(
            "the epitaph rides the last content row",
            "status 3" in row and "[re-run]" in row,
            repr(row),
        )
    os.unlink(cfg)


def test_compact_float_keeps_the_classic_frame():
    with Session(SH, cols=64, rows=22, config=COMPACT, layout=GRID) as s:
        s.settle(60)
        s.key("f")
        s.settle(30)
        f = s.focused()
        check(
            "a float's contents sit inside its own border",
            f["content_x"] == f["x"] + 1 and f["content_y"] == f["y"] + 1,
            str(f),
        )
        s.key("f")
        s.settle(30)


def test_compact_is_a_config_key():
    import subprocess

    from harness import BIN

    env = dict(os.environ, SLOSH_CONFIG="/nonexistent/slosh.kdl")
    out = subprocess.run(
        [BIN, "--dump-config"], capture_output=True, text=True, env=env
    ).stdout
    check(
        "dump-config writes the knob",
        any(l.split() and l.split()[0] == "compact" for l in out.splitlines()),
        out[:200],
    )
    r = subprocess.run(
        [BIN, "--check", COMPACT], capture_output=True, text=True, env=env
    )
    check("and --check accepts compact true", r.returncode == 0, r.stdout + r.stderr)


if __name__ == "__main__":
    test_gap0_border_drag_moves_the_boundary()
    test_gap0_rim_drags_but_never_splits()
    test_gap0_handle_click_still_splits()
    test_compact_boundary_costs_one_cell()
    test_compact_junctions_are_real_glyphs()
    test_compact_titles_ride_the_shared_line()
    test_compact_dividers_drag_like_gaps()
    test_compact_interior_edges_resize_but_do_not_split()
    test_compact_outer_frame_still_splits()
    test_compact_hover_hint_spares_the_title()
    test_compact_focus_ring_wears_the_frame_colour()
    test_compact_dead_pane_epitaph_overlays_the_last_row()
    test_compact_float_keeps_the_classic_frame()
    test_compact_is_a_config_key()
    sys.exit(report())
