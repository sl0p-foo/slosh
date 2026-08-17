#!/usr/bin/env python3
"""Shaders whose strength is an expression, not a number.

`amount=90` and `amount="(x < 10) * 200"` are the same key and the same idea,
one of them constant. What is being checked here is that the expression really
reaches the cells, that a broken one costs its own shader and nothing else,
and that the states table gets the same treatment as the global chain.
"""

import sys
import tempfile

from harness import Session, check, report

# Prints a full row of known-coloured text, then waits.
SH = [
    "/bin/sh",
    "-c",
    'printf "\\033]2;p\\007"; printf "%s\\n" ABCDEFGHIJKLMNOPQRSTUVWXYZ; read x',
]


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    # A known default_fg, so "what colour is this cell" has one answer.
    f.write('theme { default_fg "#ffffff" default_bg "#000000" }\n' + text)
    f.close()
    return f.name


# `None` means the cell carries no colour of its own -- the terminal's default,
# which is what a cell the pass never touched still looks like. A cell whose
# strength worked out to zero reads as None rather than as our idea of the
# default, because a pass at zero strength is skipped rather than run to no
# effect: that is the difference between "left alone" and "repainted the same".
def fg_row(snap, pane, row=0):
    y = pane["content_y"] + row
    return [
        (snap.style_at(pane["content_x"] + x, y) or {}).get("fg")
        for x in range(pane["content_w"])
    ]


def test_an_expression_decides_the_amount_per_cell():
    c = cfg('shaders {\n    dim amount="(x < 10) * 200"\n}\n')
    with Session(SH, cols=44, rows=8, config=c) as s:
        snap = s.until_text("ABC")
        row = fg_row(snap, s.pane())
        check(
            "the first ten columns are dimmed",
            all(c == row[0] and c != "#ffffff" for c in row[:10]),
            str(row[:12]),
        )
        check(
            "and the rest are untouched, not repainted the same",
            all(c is None for c in row[10:20]),
            str(row[10:20]),
        )


def test_a_constant_expression_is_just_a_number():
    """Folded at compile time, so it has to behave identically to writing it."""
    a = cfg('shaders {\n    dim amount="100 + 28"\n}\n')
    b = cfg("shaders {\n    dim amount=128\n}\n")
    with (
        Session(SH, cols=44, rows=8, config=a) as s1,
        Session(SH, cols=44, rows=8, config=b) as s2,
    ):
        r1 = fg_row(s1.until_text("ABC"), s1.pane())
        r2 = fg_row(s2.until_text("ABC"), s2.pane())
        check(
            "an expression that reads nothing matches the number",
            r1 == r2,
            f"{r1[:4]} vs {r2[:4]}",
        )


def test_the_rect_is_known_to_the_expression():
    """`cols` is a dependency even though it never varies per cell: it is not
    known at compile time, and folding it there made every size read as 0."""
    c = cfg('shaders {\n    dim amount="(x > cols - 5) * 200"\n}\n')
    with Session(SH, cols=44, rows=8, config=c) as s:
        snap = s.until_text("ABC")
        pane = s.pane()
        row = fg_row(snap, pane)
        check(
            "the last columns are dimmed, not the first",
            row[0] is None and row[-1] not in (None, "#ffffff"),
            f"{row[0]} .. {row[-1]}",
        )


def test_it_follows_a_resize():
    c = cfg('shaders {\n    dim amount="(x > cols - 5) * 200"\n}\n')
    with Session(SH, cols=60, rows=8, config=c) as s:
        s.until_text("ABC")
        s.resize(40, 8)
        s.settle()
        snap = s.snapshot()
        pane = s.pane()
        row = fg_row(snap, pane)
        # The cached map has to be rebuilt for the new rect, or the dimmed
        # band would still be sitting where the old right-hand edge was.
        check(
            "the band moved with the edge",
            row[-1] not in (None, "#ffffff") and row[len(row) - 8] is None,
            str(row[-10:]),
        )


def test_a_broken_expression_costs_only_its_own_shader():
    c = cfg(
        "shaders {\n"
        '    dim amount="nosuchthing * 2"\n'
        '    tint color="#00ff00" amount="255"\n'
        "}\n"
    )
    with Session(SH, cols=44, rows=8, config=c) as s:
        snap = s.until_text("ABC")
        check("the session starts anyway", s.alive())
        row = fg_row(snap, s.pane())
        check("and the shader after it still ran", row[0] == "#00ff00", str(row[:3]))


def test_states_take_expressions_too():
    c = cfg('states {\n    unfocused { dim amount="(y % 2) * 200" }\n}\n')
    with Session(SH, cols=80, rows=16, config=c) as s:
        s.until_text("ABC")
        s.key("\\\\")  # split; the left pane loses focus
        s.settle()
        panes = s.panes()
        unfocused = [p for p in panes if not p["focused"]][0]
        snap = s.snapshot()
        r0 = fg_row(snap, unfocused, 0)
        r1 = fg_row(snap, unfocused, 1)
        check("even rows are left alone", r0[0] is None, str(r0[:3]))
        check(
            "odd rows are dimmed", r1[0] != "#ffffff" and r1[0] is not None, str(r1[:3])
        )


def test_an_expression_cannot_hang_the_session():
    """There is no loop to write, so the worst case is a long expression --
    and one longer than the program limit is refused rather than run."""
    long_expr = "+".join(["x"] * 300)
    c = cfg('shaders {\n    dim amount="%s"\n}\n' % long_expr)
    with Session(SH, cols=44, rows=8, config=c) as s:
        snap = s.until_text("ABC")
        check("the session is alive and drawing", s.alive())
        # Unstyled, not merely undimmed: with its only shader refused there
        # is no pass at all, so the cells still defer to the terminal.
        check(
            "and the refused shader did nothing",
            fg_row(snap, s.pane())[0] in (None, "#ffffff"),
            str(fg_row(snap, s.pane())[:3]),
        )


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
