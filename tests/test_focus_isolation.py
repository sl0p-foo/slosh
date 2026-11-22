#!/usr/bin/env python3
"""Background operations cannot reassign the visible tab's keyboard focus."""

import sys

from harness import Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]


def setup(s):
    s.settle()
    front = s.focused()
    s.api("set-name", target="tab", id=front["tab_id"], name="foreground-tab")
    s.api("set-name", target="pane", id=front["id"], name="foreground-pane")
    made = s.api(
        "apply-layout",
        focus=False,
        kdl='layout { tab name="background-tab" { pane suspended=true } }',
    )
    check("background layout was created", made.get("ok") is True, str(made))
    back = next(p for p in s.panes() if p["tab_id"] != front["tab_id"])
    s.api("set-name", target="pane", id=back["id"], name="background-pane")
    return front, back


def still_here(s, front, label):
    panes = s.panes()
    tabs = s.tabs()
    snap = s.snapshot()
    focused = [p for p in panes if p["focused"]]
    check(
        label + ": one focused pane in active tab",
        len(focused) == 1
        and focused[0]["id"] == front["id"]
        and any(t["active"] and t["id"] == front["tab_id"] for t in tabs),
        f"{focused} {tabs}",
    )
    status = snap.line(snap.rows - 2)
    check(
        label + ": title stays on foreground pane",
        "foreground-pane" in status and "background-pane" not in status,
        repr(status),
    )


def test_rerun_background_suspended_pane():
    with Session(SH, cols=90, rows=18) as s:
        front, back = setup(s)
        answer = s.api("rerun", id=back["id"])
        check(
            "background rerun returns its target id",
            answer.get("ok") is True and answer.get("id") == back["id"],
            str(answer),
        )
        still_here(s, front, "background rerun")
        s.api(
            "raw",
            id=back["id"],
            data="\x1b]2;background-updated\x07\x1b]5577;1;status;working\x1b\\",
        )
        s.until(
            lambda _: any(
                p["id"] == back["id"] and p["status"] == "working" for p in s.panes()
            )
        )
        still_here(s, front, "background title and status update")
        s.api("close", id=back["id"])
        still_here(s, front, "closing rerun background tab")


def test_failed_background_layout_does_not_change_next_request():
    with Session(SH, cols=90, rows=18) as s:
        s.settle()
        failed = s.api("apply-layout", focus=False, kdl="layout {")
        check("malformed background layout refused", failed.get("ok") is False)
        made = s.api("apply-layout", kdl='layout { tab name="next" { pane } }')
        check("subsequent default layout succeeded", made.get("ok") is True)
        check(
            "failed request did not keep view on subsequent layout",
            next(t for t in s.tabs() if t["active"])["name"] == "next",
            str(s.tabs()),
        )
        failed = s.api("apply-layout", focus=False, path="/nonexistent/slosh.layout")
        check("missing background layout refused", failed.get("ok") is False)
        failed = s.api("apply-layout", focus=False)
        check("background layout without a source refused", failed.get("ok") is False)
        s.api("apply-layout", kdl='layout { tab name="after" { pane } }')
        check(
            "failed file and missing source do not leak focus:false",
            next(t for t in s.tabs() if t["active"])["name"] == "after",
            str(s.tabs()),
        )


def test_float_background_pane():
    with Session(SH, cols=90, rows=18) as s:
        front, back = setup(s)
        made = s.api("split", id=back["id"], focus=False)
        check("background split succeeded", made.get("ok") is True, str(made))
        target = made["id"]
        answer = s.api("float", id=target)
        check("background float succeeded", answer.get("ok") is True, str(answer))
        still_here(s, front, "background float")
        answer = s.api("float", id=target, x=8, y=5, w=24, h=8)
        check("background placement succeeded", answer.get("ok") is True, str(answer))
        still_here(s, front, "background placement")


if __name__ == "__main__":
    test_rerun_background_suspended_pane()
    test_float_background_pane()
    test_failed_background_layout_does_not_change_next_request()
    sys.exit(report())
