#!/usr/bin/env python3
"""Select-to-copy, middle-click paste, and toasts.

The clipboard lives on the *client's* machine, so a copy travels to it as
OSC 52; the session keeps its own copy as the primary selection, which is what
middle click pastes.
"""
import sys
import time

from harness import Session, check, report

# Not an interactive shell: readline treats a typed ESC as a meta prefix and
# eats it, so `printf "<ESC>]52;..."` never reaches printf. A raw read loop
# passes bytes through untouched. (The same trap as the OSC 5577 tests.)
SH = ["/bin/sh", "-c",
      'stty raw -echo; while IFS= read -r l; do eval "$l"; done']


def echoing(preamble):
    """A pane that prints something, then echoes whatever is sent to it."""
    return ["/bin/sh", "-c", f'{preamble}; stty raw -echo; cat']


def emits(*seqs):
    """A pane that prints escape sequences itself, with printf doing the
    escaping — no driver quoting, no shell line editor in the way."""
    body = "".join('printf "%s";' % q.replace("%", "%%") for q in seqs)
    return ["/bin/sh", "-c", body + " sleep 5"]


def content(s, p=None):
    p = p or s.pane()
    return p["content_x"], p["content_y"]


def select(s, x0, y0, x1, y1):
    """Drag from one screen cell to another with the left button held."""
    s.send(rf"\e[<0;{x0 + 1};{y0 + 1}M")
    s.send(rf"\e[<32;{x1 + 1};{y1 + 1}M")
    s.send(rf"\e[<0;{x1 + 1};{y1 + 1}m")


def test_select_to_copy():
    with Session(SH, cols=46, rows=10) as s:
        s.settle()
        s.raw(r'printf "hello selection world\\n"' + "\\n")
        s.settle(200)
        cx, cy = content(s)
        row = cy  # no prompt in this pane: our line is the first one

        select(s, cx, row, cx + 12, row)
        s.settle(80)
        clip = s.api("clipboard")["text"]
        check("releasing the drag copies what was under it",
              clip == "hello selecti", repr(clip))

        check("and says so", "copied 13 chars" in s.snapshot().screen(),
              repr(s.snapshot().screen()[-120:]))


def test_selection_is_visible():
    with Session(SH, cols=46, rows=10) as s:
        s.settle()
        s.raw(r'printf "highlight me\\n"' + "\\n")
        s.settle(200)
        cx, cy = content(s)
        row = cy

        s.send(rf"\e[<0;{cx + 1};{row + 1}M")
        s.send(rf"\e[<32;{cx + 9};{row + 1}M")
        s.settle(80)
        snap = s.snapshot()
        runs = [r for r in snap.styles if "inverse" in r["attrs"]]
        check("the selection is visible while dragging", runs != [], str(snap.styles))
        if runs:
            check("and covers what was dragged over",
                  runs[0]["y"] == row and runs[0]["w"] >= 8, str(runs[0]))
        s.send(rf"\e[<0;{cx + 9};{row + 1}m")


def test_middle_click_pastes():
    # a pane that echoes, so a paste is visible; the eval-loop pane would try
    # to *run* what was pasted, which is a different (and worse) demonstration
    with Session(echoing(r'printf "paste-me-please\n"'), cols=46, rows=10) as s:
        s.settle(200)
        cx, cy = content(s)
        select(s, cx, cy, cx + 14, cy)
        s.settle(80)
        check("something was copied", s.api("clipboard")["text"] == "paste-me-please",
              repr(s.api("clipboard")["text"]))

        s.send(rf"\e[<1;{cx + 1};{cy + 3}M")  # middle button
        s.send(rf"\e[<1;{cx + 1};{cy + 3}m")
        s.settle(150)
        text = s.snapshot().pane_text(s.pane())
        check("middle click pastes the selection into the pane",
              text.count("paste-me-please") >= 2, repr(text[:120]))


def test_a_program_can_copy():
    """OSC 52 from inside a pane reaches the same clipboard."""
    with Session(SH, cols=50, rows=10) as s:
        s.settle()
        # base64 of "from-the-pane"
        s.raw(r'printf "\e]52;c;ZnJvbS10aGUtcGFuZQ==\x07"' + "\\n")
        s.settle(200)
        check("a pane writing OSC 52 sets the clipboard",
              s.api("clipboard")["text"] == "from-the-pane",
              repr(s.api("clipboard")["text"]))


def test_toasts():
    # A toast's lifetime is a real duration, so this really does wait for it -
    # but the duration is configurable, so it waits 150ms instead of 2500.
    import tempfile
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write("toast_ms 150\n")
    f.close()
    with Session(SH, cols=50, rows=10, config=f.name) as s:
        s.settle()
        r = s.api("notify", text="build finished")
        check("notify is accepted", r["ok"], str(r))
        check("and appears on screen", "build finished" in s.snapshot().screen(),
              repr(s.snapshot().screen()[-120:]))
        check("bottom right, out of the way",
              "build finished" in s.snapshot().text[-1], repr(s.snapshot().text[-1]))

        s.api("notify", text="second thing")
        snap = s.snapshot()
        check("several stack up",
              "build finished" in snap.screen() and "second thing" in snap.screen(),
              repr(snap.screen()[-200:]))

        time.sleep(0.25)
        s.settle(20)
        check("and they expire on their own",
              "build finished" not in s.snapshot().screen(),
              repr(s.snapshot().screen()[-120:]))


def test_pane_notifications_become_toasts():
    with Session(SH, cols=50, rows=10) as s:
        s.settle()
        s.raw(r'printf "\e]9;agent needs you\x07"' + "\\n")
        s.settle(200)
        check("OSC 9 from a pane becomes a toast",
              "agent needs you" in s.snapshot().screen(),
              repr(s.snapshot().screen()[-160:]))


def test_selection_does_not_fight_a_mouse_program():
    tracker = ["/bin/sh", "-c",
               'stty raw -echo; printf "\\033[?1000h\\033[?1006h"; cat -v']
    with Session(tracker, cols=50, rows=10) as s:
        s.settle()
        cx, cy = content(s)
        select(s, cx + 1, cy + 1, cx + 6, cy + 1)
        s.settle(80)
        check("a mouse-tracking program keeps its own clicks",
              s.api("clipboard")["text"] == "", repr(s.api("clipboard")["text"]))
        check("and receives them", "^[[<0;" in s.snapshot().pane_text(s.pane()),
              repr(s.snapshot().pane_text(s.pane())[:100]))


if __name__ == "__main__":
    test_select_to_copy()
    test_selection_is_visible()
    test_middle_click_pastes()
    test_a_program_can_copy()
    test_toasts()
    test_pane_notifications_become_toasts()
    test_selection_does_not_fight_a_mouse_program()
    sys.exit(report())
