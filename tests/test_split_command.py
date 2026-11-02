#!/usr/bin/env python3
"""`split` with a `command`: one call for "a pane running this, beside me".

The bug this pins: a caller that wanted a pane running something *in its own
tab* could not ask for one. `apply-layout` is the only verb that knows
`command=`, and it only ever appends tabs -- a bare `pane` in a layout is
refused outright ("layout declares no tabs") -- so the documented way to run
background work was to build it in a tab of its own and `move-pane` it in
afterwards.

That is two calls with a tab visible in the strip between them, and the second
one is the one that goes wrong: pane ids are not sequential (a split node takes
one), so a caller guessing the id gets "cannot move it there" and leaves its
pane stranded in a tab nobody asked for. Which is exactly what agents driving
this socket did, every time.

`split` already took the tab it was splitting into -- `place_beside` has always
been parameterised by it -- so the destination was never the hard part. What it
lacked was the layout file's vocabulary, and a way not to take focus:

  * `command` runs something, instead of making a shell.
  * `cwd` says where, instead of the split pane's own directory.
  * `purpose` tags it, declared, so the pane cannot relabel itself.
  * `focus:false` builds it without pulling the view -- and the reply still
    carries the new pane's `id`, since there is then no focus to ask.
  * the pane lands in the *split pane's* tab, which need not be on screen, and
    it is laid out there (a background pane used to come up 1x1).

The old two-argument form still means what it meant: a shell, focused.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Session, check, report

SH = ["/bin/sh", "-c", "read x"]
TALKER = ["/bin/sh", "-c", "echo MARKER-ONE; read x"]


def viewing(s):
    return [t["id"] for t in s.tabs() if t["active"]]


def by_purpose(s, purpose):
    return [p for p in s.panes() if p["purpose"] == purpose]


def test_a_commanded_pane_lands_beside_the_pane_it_split():
    """The whole point: no new tab, and the reply names the pane."""
    with Session(TALKER, cols=80, rows=20) as s:
        s.settle(100)
        me = s.pane(0)
        tabs_before = len(s.tabs())

        reply = s.api(
            "split",
            id=me["id"],
            dir="rows",
            command="echo MARKER-BUILT; read x",
            purpose="task:build",
            focus=False,
        )
        s.until_text("MARKER-BUILT")

        check("the split is accepted", reply.get("ok") is True, str(reply))
        check(
            "no tab was created to hold it",
            len(s.tabs()) == tabs_before,
            f"{tabs_before} tabs before, {len(s.tabs())} after",
        )
        built = by_purpose(s, "task:build")
        check("the pane exists", len(built) == 1, str(s.panes()))
        if not built:
            return
        check(
            "the reply named it, so nobody has to guess an id",
            reply.get("id") == built[0]["id"],
            f"reply said {reply.get('id')}, the pane is {built[0]['id']}",
        )
        check(
            "it is in the same tab as the pane it split",
            built[0]["tab_id"] == me["tab_id"],
            f"split pane is in tab {me['tab_id']}, the new one in {built[0]['tab_id']}",
        )
        check(
            "and it ran the command rather than a shell",
            s.api("capture", id=built[0]["id"]).get("text", "").strip()
            == "MARKER-BUILT",
            str(s.api("capture", id=built[0]["id"])),
        )


def test_it_does_not_pull_the_view_or_the_focus():
    """`focus:false`, from a tab that is not the one being split.

    The case that matters and the one the old verb could not express: the
    caller lives in a tab nobody is looking at. `app_split_pane` used to
    `app_focus_pane` the pane first, which *selects that pane's tab* -- so
    splitting yourself yanked the human's view into your tab before you had
    done anything."""
    with Session(TALKER, cols=80, rows=20) as s:
        s.settle(100)
        mine = s.pane(0)
        s.api("new-tab", name="human-tab")
        s.api("select-tab", index=2)
        s.settle(50)
        was_viewing, was_focused = viewing(s), s.focused()["id"]

        s.api(
            "split",
            id=mine["id"],
            command="echo MARKER-BG; read x",
            purpose="task:bg",
            focus=False,
        )
        s.until_text("MARKER-ONE")  # the human's tab is still the one composited

        check(
            "the view stays on the tab it was on",
            viewing(s) == was_viewing,
            f"was viewing {was_viewing}, now viewing {viewing(s)}",
        )
        check(
            "and the focused pane is still the human's",
            s.focused()["id"] == was_focused,
            f"focus was {was_focused}, now {s.focused()['id']}",
        )
        built = by_purpose(s, "task:bg")
        check("the pane was still built", len(built) == 1, str(s.panes()))
        if built:
            check(
                "in the caller's tab, not the one on screen",
                built[0]["tab_id"] == mine["tab_id"],
                str(built[0]),
            )
            # The failure that hid for as long as making a pane always jumped
            # the view to it: a tab nobody looked at never had a layout pass.
            check(
                "and it was laid out there, not left at 1x1",
                built[0]["content_w"] > 1 and built[0]["content_h"] > 1,
                f"{built[0]['content_w']}x{built[0]['content_h']}",
            )


def test_the_purpose_is_declared():
    """A caller's handle on its own pane, which a program in it must not take."""
    with Session(SH, cols=80, rows=20) as s:
        s.settle(100)
        s.api("split", id=s.pane(0)["id"], purpose="task:mine", focus=False)
        s.settle(50)
        built = by_purpose(s, "task:mine")
        check("the purpose is set", len(built) == 1, str(s.panes()))
        if built:
            check(
                "and declared, so in-band output cannot relabel it",
                built[0]["purpose_declared"] is True,
                str(built[0]),
            )
            check(
                "an in-band attempt on it is refused",
                not s.api(
                    "set-purpose", id=built[0]["id"], purpose="hijacked", declared=False
                ).get("ok"),
                "an in-band purpose overwrote a declared one",
            )


def test_cwd_says_where():
    with Session(SH, cols=80, rows=20) as s:
        s.settle(100)
        r = s.api(
            "split", id=s.pane(0)["id"], cwd="/tmp", command="pwd; read x", focus=False
        )
        s.until_text("/tmp")
        check(
            "the command ran in the directory asked for",
            s.api("capture", id=r.get("id")).get("text", "").strip() == "/tmp",
            str(s.api("capture", id=r.get("id"))),
        )


def test_the_old_form_still_means_what_it_meant():
    """Two arguments: a shell, focused, and `id` is the new pane either way."""
    with Session(SH, cols=80, rows=20) as s:
        s.settle(100)
        before = {p["id"] for p in s.panes()}
        reply = s.api("split", dir="cols")
        s.settle(50)
        after = {p["id"] for p in s.panes()}
        fresh = after - before
        check("a pane was made", len(fresh) == 1, f"{before} -> {after}")
        check(
            "the reply names it, as it always did",
            reply.get("id") in fresh,
            f"reply {reply}, new panes {fresh}",
        )
        check(
            "and it has the focus, as it always did",
            s.focused()["id"] in fresh,
            f"focused {s.focused()['id']}, new panes {fresh}",
        )
        check(
            "with no purpose, having been given none",
            [p["purpose"] for p in s.panes() if p["id"] in fresh] == [""],
            str(s.panes()),
        )


def test_the_refusals_still_refuse():
    with Session(SH, cols=80, rows=20) as s:
        s.settle(100)
        me = s.pane(0)["id"]
        check(
            "a pane that does not exist",
            s.api("split", id=99999, command="true").get("error") == "no such pane",
            str(s.api("split", id=99999, command="true")),
        )
        # A second pane first: a tab whose every pane floats self-heals, so a
        # lone pane asked to float stays tiled and would split just fine.
        other = s.api("split", id=me, focus=False).get("id")
        s.api("float", id=other)
        s.settle(50)
        floated = [p for p in s.panes() if p["id"] == other]
        check(
            "the pane really is floating",
            floated and floated[0]["floating"] is True,
            str(floated),
        )
        check(
            "and a floating pane, which has no beside",
            s.api("split", id=other, command="true").get("error")
            == "cannot split a floating pane",
            str(s.api("split", id=other, command="true")),
        )


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
