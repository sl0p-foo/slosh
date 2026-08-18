#!/usr/bin/env python3
"""Writing a session back out as a layout, and getting it back.

This is what makes a restart survivable: build a new binary, dump what you
have, quit, come back with --layout. contrib/slosh-dev is that loop.

The only assertion worth much here is the round trip. A dump that *looks*
right and rebuilds a different tree is exactly the failure this would have —
the first version wrote `split=` on the children instead of on the node that
has them, which loaded without complaint and produced the wrong shape.
"""

import os
import sys
import tempfile

from harness import Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]

LAYOUT = """
layout {
    tab name="api" purpose="project:api" cwd="/tmp" {
        pane command="stty raw -echo; cat" purpose="agent:main"
        pane split="rows" {
            pane command="stty raw -echo; cat" suspended=true
            pane
        }
    }
    tab name="notes" {
        pane
    }
}
"""


def write(text, suffix=".kdl"):
    f = tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False)
    f.write(text)
    f.close()
    return f.name


def shape(s):
    """The structure, as something two sessions can be compared on."""
    return [
        (
            p["tab"],
            p["x"],
            p["y"],
            p["w"],
            p["h"],
            p["purpose"],
            p["suspended"],
            p["focused"],
        )
        for p in s.panes()
    ]


def test_a_dump_reloads_into_the_same_session():
    lay = write(LAYOUT)
    with Session(SH, cols=90, rows=20, layout=lay) as first:
        first.settle(60)
        # Move a boundary and change tabs, so there is something to lose.
        first.send(r"\x01L")
        first.settle(20)
        before = shape(first)
        dumped = first.api("dump-layout")["kdl"]

    check("the dump is a layout", dumped.startswith("layout {"), dumped[:60])

    again = write(dumped)
    with Session(SH, cols=90, rows=20, layout=again) as second:
        second.settle(60)
        after = shape(second)

    check(
        "the same panes, in the same places",
        before == after,
        "\n  before %s\n  after  %s" % (before, after),
    )
    os.unlink(lay)
    os.unlink(again)


def test_dumping_the_reload_gives_the_same_text():
    """Idempotence: dump, load, dump again. If the two differ, one of them is
    losing something, and a diff of layout files should be about what changed
    in the session rather than about the dumper."""
    lay = write(LAYOUT)
    with Session(SH, cols=90, rows=20, layout=lay) as first:
        first.settle(60)
        one = first.api("dump-layout")["kdl"]
    again = write(one)
    with Session(SH, cols=90, rows=20, layout=again) as second:
        second.settle(60)
        two = second.api("dump-layout")["kdl"]
    check(
        "a dump of a dump is the same dump",
        one == two,
        "\n--- first\n%s\n--- second\n%s" % (one, two),
    )
    os.unlink(lay)
    os.unlink(again)


def test_it_records_where_the_shell_actually_is():
    """Not where the pane was started: after `cd` those are different, and the
    useful one is the one the kernel knows."""
    argv = ["/bin/sh", "-c", "cd /usr; stty raw -echo; cat"]
    with Session(argv, cols=60, rows=10) as s:
        s.settle(80)
        dumped = s.api("dump-layout")["kdl"]
        check("the dump follows the shell", 'cwd="/usr"' in dumped, dumped)


def test_proportions_and_focus_come_back():
    with Session(SH, cols=100, rows=20) as s:
        s.settle()
        s.key("\\\\")
        s.settle()
        for _ in range(3):
            s.send(r"\x01L")  # push the boundary well off centre
        s.settle()
        widths = [p["w"] for p in s.panes()]
        focused = [i for i, p in enumerate(s.panes()) if p["focused"]][0]
        dumped = s.api("dump-layout")["kdl"]
        check("the dump carries weights", "weight=" in dumped, dumped)
        check("and which pane was focused", "focus=true" in dumped, dumped)

    lay = write(dumped)
    with Session(SH, cols=100, rows=20, layout=lay) as s2:
        s2.settle(60)
        check(
            "the proportions survive",
            [p["w"] for p in s2.panes()] == widths,
            f"{[p['w'] for p in s2.panes()]} vs {widths}",
        )
        check(
            "and so does the focus",
            [i for i, p in enumerate(s2.panes()) if p["focused"]] == [focused],
            str([p["focused"] for p in s2.panes()]),
        )
    os.unlink(lay)


def test_the_active_tab_survives():
    lay = write(LAYOUT)
    with Session(SH, cols=90, rows=20, layout=lay) as s:
        s.settle(60)
        s.key(r"\t")  # C-a tab: to the second tab
        s.settle()
        was = [t["index"] for t in s.tabs() if t["active"]]
        dumped = s.api("dump-layout")["kdl"]
        check("the dump says which tab was active", "active=true" in dumped, dumped)
    again = write(dumped)
    with Session(SH, cols=90, rows=20, layout=again) as s2:
        s2.settle(60)
        check(
            "and it comes back on that tab",
            [t["index"] for t in s2.tabs() if t["active"]] == was,
            str(s2.tabs()),
        )
    os.unlink(lay)
    os.unlink(again)


def test_a_plain_shell_pane_comes_back_as_a_shell():
    """Not as a re-run of one: a pane with no command in the layout has none in
    the dump, or every restart would wrap the shell in another shell."""
    with Session(SH, cols=60, rows=10) as s:
        s.settle()
        dumped = s.api("dump-layout")["kdl"]
        check("no command is invented for it", "command=" not in dumped, dumped)


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
