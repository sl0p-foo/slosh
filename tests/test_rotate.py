#!/usr/bin/env python3
"""`rotate-layout`: a quarter turn of the tab, clockwise.

The whole feature is one asymmetry. Turn a *column* of panes clockwise and the
one that was on top is now the one on the right, so a row split becomes a column
split read backwards; turn a *row* of them and the leftmost becomes the top, so
the order stands. Get that backwards and you have a mirror image, which looks
almost right and is not a rotation — the tell is that four turns no longer come
back to where you started, which is what most of this file checks.
"""
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]


def rects(s):
    """Every pane as (id, x, y, w, h), in reading order."""
    return [(p["id"], p["x"], p["y"], p["w"], p["h"])
            for p in sorted(s.panes(), key=lambda p: (p["y"], p["x"]))]


def turn(s, n=1):
    for _ in range(n):
        s.key(r"\x20")   # C-a space: the escape, so the driver's line keeps it
        s.settle()


def test_two_rows_become_two_columns_the_right_way_round():
    with Session(SH, cols=80, rows=24) as s:
        s.settle()
        s.key("-")            # rows: the first pane on top, the new one below
        s.settle()
        top, bottom = rects(s)
        check("it starts as two rows", top[2] < bottom[2] and top[3] == bottom[3],
              str(rects(s)))

        turn(s)
        left, right = sorted(rects(s), key=lambda r: r[1])
        check("one turn makes two columns",
              left[2] == right[2] and left[1] < right[1], str(rects(s)))
        check("and the pane that was on top is now on the right",
              right[0] == top[0], f"top was {top[0]}, right is {right[0]}")
        check("the one that was underneath is on the left",
              left[0] == bottom[0], f"bottom was {bottom[0]}, left is {left[0]}")

        turn(s)
        a, b = rects(s)
        check("two turns are two rows again", a[3] == b[3] and a[2] < b[2],
              str(rects(s)))
        check("...with the panes the other way up than they started",
              a[0] == bottom[0] and b[0] == top[0], str(rects(s)))


def test_four_turns_are_the_identity():
    """The property that makes this safe to press: it is a permutation of the
    tree and nothing else, so pressing it enough times undoes it exactly."""
    with Session(SH, cols=100, rows=30) as s:
        s.settle()
        s.key("\\\\")   # a nest: one pane beside a stack of two
        s.key("-")
        s.settle()
        start = rects(s)
        check("three panes to turn", len(start) == 3, str(start))

        seen = [start]
        for i in range(1, 4):
            turn(s)
            seen.append(rects(s))
            check(f"turn {i} changes the arrangement", seen[-1] != start,
                  str(seen[-1]))
        turn(s)
        check("the fourth turn comes back exactly", rects(s) == start,
              f"{start} -> {rects(s)}")


def test_a_share_of_one_axis_becomes_a_share_of_the_other():
    with Session(SH, cols=100, rows=30) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        for _ in range(4):
            s.key("L")     # push the boundary right: the left pane grows
        s.settle()
        left, right = sorted(rects(s), key=lambda r: r[1])
        check("the columns are uneven", left[3] > right[3] + 8,
              f"{left[3]} vs {right[3]}")
        ratio = left[3] / right[3]

        turn(s)
        top, bottom = sorted(rects(s), key=lambda r: r[2])
        check("the wider pane is now the taller one",
              top[0] == left[0] and top[4] > bottom[4],
              f"{rects(s)} (left was {left[0]})")
        check("and it kept roughly the share it had",
              abs(top[4] / bottom[4] - ratio) < 0.35,
              f"{ratio:.2f} across -> {top[4] / bottom[4]:.2f} down")


def test_focus_stays_on_the_pane_you_were_in():
    with Session(SH, cols=90, rows=24) as s:
        s.settle()
        s.key("-")
        s.settle()
        was = s.focused()["id"]
        turn(s, 3)
        check("the pane you were in is the pane you are in",
              s.focused()["id"] == was, f"{was} -> {s.focused()['id']}")


def test_a_turn_that_would_not_fit_is_refused():
    """A share of the height is not always a share the width can afford, and a
    node that cannot give its children their floor takes the whole tab down to a
    list (D6). An action a person just asked for must not be what does that."""
    with Session(SH, cols=100, rows=30) as s:
        s.settle()
        s.key("-")
        s.settle()
        for _ in range(5):
            s.key("K")     # squeeze the top pane: fine as a row, too thin as a column
        s.settle()
        before = rects(s)

        turn(s)
        check("the layout is exactly as it was", rects(s) == before,
              f"{before} -> {rects(s)}")
        check("and it says why",
              "no room to turn it" in s.snapshot().screen(),
              repr(s.snapshot().screen()[-200:]))

        s.key("=")         # even shares fit either way round
        s.settle()
        turn(s)
        cols = sorted(rects(s), key=lambda r: r[1])
        check("evening it out first lets the same turn through",
              cols[0][2] == cols[1][2] and cols[0][1] < cols[1][1], str(rects(s)))


def test_one_pane_has_nothing_to_turn():
    with Session(SH, cols=80, rows=24) as s:
        s.settle()
        before = rects(s)
        turn(s)
        check("it says so", "nothing to turn" in s.snapshot().screen(),
              repr(s.snapshot().screen()[-200:]))
        check("and the pane is untouched", rects(s) == before,
              f"{before} -> {rects(s)}")


def test_it_is_offered_by_name():
    with Session(SH, cols=110, rows=40) as s:
        s.settle()
        s.key("?")
        s.settle()
        check("the cheatsheet lists it with its key",
              "space  turn the layout a quarter" in s.snapshot().screen(),
              repr(s.snapshot().screen()[:400]))
        s.send("q")

        s.key("p")
        s.send("rotate")
        s.settle()
        snap = s.snapshot()
        check("the palette finds it by its config name",
              "turn the layout a quarter" in snap.screen(),
              repr(snap.screen()[:400]))
        check("and the row runs it",
              any(h["action"].startswith("run:") for h in snap.hits),
              str([h["action"] for h in snap.hits][:8]))


def test_a_rotated_layout_survives_a_dump():
    """The dump is the tree, so a turn has to come back as a turn."""
    with Session(SH, cols=90, rows=24) as s:
        s.settle()
        s.key("-")
        s.settle()
        turn(s)
        kdl = s.api("dump-layout")["kdl"]
        check("it dumps as a column split", 'split="cols"' in kdl, kdl)
        turned = rects(s)

    lay = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    lay.write(kdl)
    lay.close()
    with Session(SH, cols=90, rows=24, layout=lay.name) as s2:
        s2.settle(60)
        check("and comes back in the same arrangement",
              [r[1:] for r in rects(s2)] == [r[1:] for r in turned],
              f"{turned} -> {rects(s2)}")


if __name__ == "__main__":
    test_two_rows_become_two_columns_the_right_way_round()
    test_four_turns_are_the_identity()
    test_a_share_of_one_axis_becomes_a_share_of_the_other()
    test_focus_stays_on_the_pane_you_were_in()
    test_a_turn_that_would_not_fit_is_refused()
    test_one_pane_has_nothing_to_turn()
    test_it_is_offered_by_name()
    test_a_rotated_layout_survives_a_dump()
    sys.exit(report())
