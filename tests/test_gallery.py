#!/usr/bin/env python3
"""Cross-tab gallery: live previews without moving any tab's focus."""

import tempfile
from pathlib import Path

from harness import Session, check, report

SH = ["/bin/sh", "-c", "stty raw -echo; cat"]


def test_gallery():
    with Session(SH, cols=100, rows=28) as s:
        s.settle()
        first = s.focused()["id"]
        s.raw("ONE")
        s.key("c")
        s.settle()
        second = s.focused()["id"]
        s.raw("TWO")
        s.key("c")
        s.settle()
        third = s.focused()["id"]
        s.api("set-name", target="tab", id=s.focused()["tab_id"], name="my-tab")
        s.raw("THREE")
        s.settle()
        # A background program can change its own title without changing the
        # tab currently on screen or the pane that has keyboard focus.
        s.api("raw", id=first, data="\x1b]2;other-title\x07")
        s.settle()
        check("background title did not steal focus", s.focused()["id"] == third)
        status = s.snapshot().line(s.rows - 2)
        check(
            "background title did not replace active status title",
            "my-tab" in status and "other-title" not in status,
            repr(status),
        )
        before = {p["id"]: (p["content_w"], p["content_h"]) for p in s.panes()}
        s.key("g")
        snap = s.snapshot()
        check(
            "all tab contents appear",
            all(t in snap.screen() for t in ("ONE", "TWO", "THREE")),
            snap.screen(),
        )
        panes = {p["id"]: p for p in s.panes()}
        sizes = [(panes[i]["w"], panes[i]["h"]) for i in (first, second, third)]
        check(
            "gallery tiles are equal-sized",
            max(w for w, _ in sizes) - min(w for w, _ in sizes) <= 1
            and max(h for _, h in sizes) - min(h for _, h in sizes) <= 1,
            str(sizes),
        )
        check("gallery did not change keyboard focus", s.focused()["id"] == third)
        s.send(r"\e")
        s.snapshot()
        restored = {p["id"]: (p["content_w"], p["content_h"]) for p in s.panes()}
        check(
            "gallery restores every pty size",
            restored == before,
            f"{restored} != {before}",
        )
        check("Escape stayed on the original tab", s.focused()["id"] == third)

        s.api("gallery")
        s.send(r"\e[D")  # left, onto tab 2
        check("selection does not change active tab", s.focused()["id"] == third)
        s.send(r"\r")
        check("Enter enters selected tab", s.focused()["id"] == second)
        s.api("gallery")
        snap = s.snapshot()
        p = next(p for p in s.panes() if p["id"] == first)
        s.click(p["x"] + p["w"] // 2, p["y"] + 1)
        check("click enters the tile's tab", s.focused()["id"] == first)


def test_background_close_preserves_active_title():
    with Session(SH, cols=80, rows=16) as s:
        s.settle()
        s.api("new-tab", name="background")
        s.api("new-tab", name="working")
        s.settle()
        current = s.focused()["id"]
        other = next(p for p in s.panes() if p["tab"] == 2)
        s.api("close", id=other["id"])
        status = s.snapshot().line(s.rows - 2)
        check(
            "closing a background tab retains active title",
            "working" in status and "background" not in status,
            repr(status),
        )
        check(
            "closing a background tab retains keyboard focus",
            s.focused()["id"] == current,
        )


def test_background_exit_preserves_title():
    """The background event that used to shift the active tab index."""
    with tempfile.TemporaryDirectory() as d:
        cfg = Path(d) / "config.kdl"
        cfg.write_text('keep_dead "none"\n')
        with Session(SH, cols=80, rows=16, config=cfg) as s:
            s.settle()
            s.api(
                "apply-layout",
                focus=False,
                kdl='layout { tab name="background" { pane command="read x" } }',
            )
            s.api("new-tab", name="working")
            s.settle()
            current = s.focused()["id"]
            other = next(p for p in s.panes() if p["tab"] == 2)
            s.api("raw", id=other["id"], data="done\n")
            snap = s.until(lambda _: len(s.tabs()) == 2)
            status = snap.line(s.rows - 2)
            check(
                "background process exit retains active title",
                "working" in status and "background" not in status,
                repr(status),
            )
            check("background process exit retains focus", s.focused()["id"] == current)


def test_gallery_pagination_and_closed_tab():
    with Session(SH, cols=44, rows=16) as s:
        s.settle()
        ids = [s.focused()["id"]]
        for _ in range(5):
            s.key("c")
            s.settle()
            ids.append(s.focused()["id"])
        s.key("g")
        check("last tab visible on its page", s.panes()[-1]["w"] > 0)
        s.api("close", id=ids[-1])
        snap = s.snapshot()
        check("closing selected tab keeps gallery usable", bool(snap.screen().strip()))
        s.send(r"\e")
        check(
            "Escape after closing selected tab has a focused pane",
            s.focused() is not None,
        )


if __name__ == "__main__":
    test_gallery()
    test_gallery_pagination_and_closed_tab()
    test_background_close_preserves_active_title()
    test_background_exit_preserves_title()
    report()
