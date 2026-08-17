"""Setting a pane's purpose from the keyboard.

A purpose is what tooling finds a pane by (D8), and it could only be declared by
a layout or set over the socket -- so every pane anybody arranged by hand had
none, and a layout dumped from a session someone built by splitting panes was a
shape with no tags in it. The editor is the one the pane title and the tab label
already share, with a third subject.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Session, check, report

SH = ["/bin/sh", "-c", "read x"]


def emit_cmd(payload):
    """A shell line, typed at a pane, that prints one OSC 5577.

    `\\e`, not `\\033`: the driver's own unescaper reads `\\0` as NUL, so a
    `\\033` written here reaches the pane as NUL + "33" and printf emits no ESC
    -- a sequence that silently never arrives, and a test that passes anyway."""
    return 'printf "\\e]5577;%s\\x07"\\n' % payload.replace("%", "%%")


def shell():
    """A pane that runs lines we send it, with no line editor to eat an ESC --
    the same fixture the OSC tests use, for the same reason."""
    return ["/bin/sh", "-c", 'stty raw -echo; while IFS= read -r l; do eval "$l"; done']


def test_a_purpose_can_be_typed_where_it_will_live():
    with Session(SH) as s:
        s.key("P")
        snap = s.snapshot()
        check(
            "the editor announces which label it is editing",
            "purpose" in snap.screen(),
            snap.screen(),
        )

        s.send("service:web")
        check(
            "what is typed shows in the title cell",
            "purpose service:web" in s.snapshot().screen(),
            s.snapshot().screen(),
        )

        s.send("\\r")
        check("enter keeps it", s.pane()["purpose"] == "service:web", str(s.pane()))
        check(
            "and it counts as declared, because an operator set it",
            s.pane()["purpose_declared"] is True,
            str(s.pane()),
        )
        check(
            "the editor is gone",
            "purpose service:web" not in s.snapshot().screen(),
            s.snapshot().screen(),
        )


def test_escape_abandons_it():
    with Session(SH) as s:
        s.key("P")
        s.send("half-typed")
        s.send("\\033")
        check(
            "escape leaves the purpose alone", s.pane()["purpose"] == "", str(s.pane())
        )


def test_it_seeds_with_what_is_there():
    """Unlike a tab's name editor, which seeds empty: here the purpose *is* the
    thing being edited, and the usual edit is adding `:2` to the end of one."""
    with Session(SH) as s:
        s.api("set-purpose", id=s.focused()["id"], purpose="agent:main")
        s.key("P")
        check(
            "the editor opens on the purpose it has",
            "purpose agent:main" in s.snapshot().screen(),
            s.snapshot().screen(),
        )
        s.send(":2\\r")
        check(
            "so an edit is an edit",
            s.pane()["purpose"] == "agent:main:2",
            str(s.pane()),
        )


def test_a_typed_purpose_outranks_the_program_in_the_pane():
    """D8's trust model, reached from the keyboard: declared means "from a layout or
    an operator", and a person at the keyboard is the operator."""
    with Session(shell(), cols=60, rows=8) as s:
        s.settle()
        s.key("P")
        s.send("project:real\\r")
        s.raw(emit_cmd("1;purpose;evil:relabelled"))
        s.settle()
        check(
            "a pane cannot relabel a purpose an operator declared",
            s.pane()["purpose"] == "project:real",
            str(s.pane()),
        )


def test_clearing_it_hands_the_label_back_to_the_program():
    """A lock held over an empty string is the one state nobody can get out of:
    the program can no longer label itself and there is nothing there to have
    outranked it. Same shape as clearing a pane's name, which gives the title
    back to whatever is running."""
    with Session(shell(), cols=60, rows=8) as s:
        s.settle()
        s.key("P")
        s.send("project:real\\r")
        check("set and locked", s.pane()["purpose_declared"] is True, str(s.pane()))

        s.key("P")
        for _ in range(len("project:real")):
            s.send("\\x7f")
        s.send("\\r")
        check("cleared", s.pane()["purpose"] == "", str(s.pane()))
        check("and unlocked", s.pane()["purpose_declared"] is False, str(s.pane()))

        s.raw(emit_cmd("1;purpose;mine-now"))
        s.settle()
        check(
            "so the pane can speak for itself again",
            s.pane()["purpose"] == "mine-now",
            str(s.pane()),
        )


def test_the_action_is_bindable_and_in_the_palette():
    with Session(SH) as s:
        s.key("p")
        s.send("purpose")
        snap = s.snapshot()
        check(
            "the palette offers it without it having to be bound",
            "tag it with a purpose" in snap.screen(),
            snap.screen(),
        )
        s.send("\\r")
        check(
            "and choosing it opens the editor",
            "purpose" in s.snapshot().screen(),
            s.snapshot().screen(),
        )


def test_id_zero_is_the_focused_pane_here_too():
    """`close`, `rerun` and `clear-shaders` all read 0 as "the focused one".
    `set-purpose` was the one addressable thing that refused it, and answered
    `refused` -- which reads as "that purpose is not allowed" rather than "say
    which pane"."""
    with Session(SH) as s:
        s.api("split", dir="rows")
        r = s.api("set-purpose", id=0, purpose="service:web")
        check("0 is accepted", r["ok"] is True, str(r))
        focused = [p for p in s.panes() if p["focused"]][0]
        check(
            "and it tagged the focused pane",
            focused["purpose"] == "service:web",
            str(focused),
        )
        others = [p for p in s.panes() if not p["focused"]]
        check("and nothing else", all(p["purpose"] == "" for p in others), str(others))

        t = s.api("set-purpose", target="tab", id=0, purpose="notes")
        check("a tab takes 0 as the one you are looking at", t["ok"] is True, str(t))
        here = [x for x in s.tabs() if x["active"]][0]
        check("and that is the tab it tagged", here["purpose"] == "notes", str(here))


def test_a_dumped_layout_carries_what_was_tagged_by_hand():
    """The reason this exists: a session you built by splitting panes and tagging
    them is a layout worth writing down."""
    with Session(SH) as s:
        s.key("P")
        s.send("agent:main\\r")
        s.key("\\\\")
        s.key("P")
        s.send("shell:scratch\\r")
        kdl = s.api("dump-layout")["kdl"]
        check(
            "both tags are in the file",
            'purpose="agent:main"' in kdl and 'purpose="shell:scratch"' in kdl,
            kdl,
        )


if __name__ == "__main__":
    for name, fn in sorted(list(globals().items())):
        if name.startswith("test_"):
            fn()
    sys.exit(report())
