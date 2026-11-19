#!/usr/bin/env python3
"""ctrl+click on a path opens it in the editor, in a pane beside it.

The click only ever takes what exists: a word that looks like a path but is
no file is still an ordinary click (a selection), so the gesture costs nothing
when it misses. A relative path is resolved where the pane *is*, not where the
session was started, and `:line:col` after it reaches the editor in the
editor's own spelling.

The "editor" here is `echo`, kept alive with `read`, so the command line it
was given is on its screen to be read back.
"""

import os
import sys
import tempfile

from harness import Session, check, report

# An editor that prints what it was asked to open, and stays up.
_BIN = tempfile.mkdtemp(prefix="slosh-open-bin-")
EDITOR = os.path.join(_BIN, "fake-editor")
with open(EDITOR, "w") as _f:
    _f.write('#!/bin/sh\necho EDIT "$@"; read x\n')
os.chmod(EDITOR, 0o755)


def workdir():
    d = tempfile.mkdtemp(prefix="slosh-open-")
    os.makedirs(os.path.join(d, "src"))
    for f in ("src/app.c", "notes.md"):
        open(os.path.join(d, f), "w").close()
    os.mkdir(os.path.join(d, ".git"))
    return d


def conf(text=""):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def shell_printing(text, cwd):
    return [
        "/bin/sh",
        "-c",
        f"cd '{cwd}'; printf '%s\\n' '{text}'; stty raw -echo; cat",
    ]


def ctrl_click(s, x, y, shift=False):
    s.click(x, y, button=16 | (4 if shift else 0))


def at(s, needle):
    """Screen coordinates of the first cell of `needle`."""
    snap = s.snapshot()
    for y in range(snap.data["rows"] if "rows" in snap.data else 50):
        try:
            line = snap.line(y)
        except Exception:
            break
        x = line.find(needle)
        if x >= 0:
            return x, y
    raise AssertionError(f"{needle!r} not on screen:\n{snap.screen()}")


def editor_text(s):
    s.until_text("EDIT ")
    return s.snapshot().screen()


def test_it_opens_in_the_editor_at_the_line():
    d = workdir()
    sh = shell_printing("src/app.c:12: warning", d)
    c = conf(f'open_command "{EDITOR} +{{line}} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("warning")
        x, y = at(s, "src/app.c")
        ctrl_click(s, x + 4, y)
        screen = editor_text(s)
        want = f"EDIT +12 {d}/src/app.c"
        check("the editor got the resolved path and the line", want in screen, screen)
        check("and it has the focus", s.focused()["id"] != 1, str(s.panes()))
        # the release did not start a selection in the pane under it
        clip = s.api("clipboard").get("text")
        check("the click copied nothing", not clip, repr(clip))


def test_the_default_editor_is_told_the_line_its_own_way():
    d = workdir()
    sh = shell_printing("see src/app.c:7:3 for it", d)
    # A wrapper whose basename is nvim, so the vi-family spelling is used.
    bindir = tempfile.mkdtemp()
    nvim = os.path.join(bindir, "nvim")
    with open(nvim, "w") as f:
        f.write('#!/bin/sh\necho EDIT "$@"; read x\n')
    os.chmod(nvim, 0o755)
    with Session(sh, cols=100, rows=30, config=conf(), env={"EDITOR": nvim}) as s:
        s.until_text("for it")
        x, y = at(s, "src/app.c")
        ctrl_click(s, x, y)
        screen = editor_text(s)
        check("nvim is given a cursor call", "+call cursor(7,3)" in screen, screen)


def test_a_word_that_is_no_file_is_an_ordinary_click():
    d = workdir()
    sh = shell_printing("src/missing.c and more", d)
    c = conf(f'open_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("and more")
        x, y = at(s, "missing")
        ctrl_click(s, x, y)
        s.settle(40)
        check("no pane opened", len(s.panes()) == 1, str(s.panes()))


def test_a_plain_click_does_not_open():
    d = workdir()
    sh = shell_printing("notes.md", d)
    c = conf(f'open_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("notes.md")
        x, y = at(s, "notes.md")
        s.click(x, y)
        s.settle(40)
        check("no pane opened", len(s.panes()) == 1, str(s.panes()))


def test_open_click_off_turns_it_off():
    d = workdir()
    sh = shell_printing("notes.md", d)
    c = conf(f'open_click "off"\nopen_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("notes.md")
        x, y = at(s, "notes.md")
        ctrl_click(s, x, y)
        s.settle(40)
        check("no pane opened", len(s.panes()) == 1, str(s.panes()))


def test_a_diff_path_resolves_from_the_git_root():
    """`git diff` prints `b/src/app.c` relative to the root, wherever you
    are inside the checkout."""
    d = workdir()
    sh = shell_printing("+++ b/app.c", os.path.join(d, "src"))
    # From src/, `b/app.c` -> `app.c` in the shell's own directory.
    c = conf(f'open_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("+++")
        x, y = at(s, "b/app.c")
        ctrl_click(s, x + 3, y)
        screen = editor_text(s)
        check("the a/b prefix is dropped", f"EDIT {d}/src/app.c" in screen, screen)

    sh = shell_printing("notes.md", os.path.join(d, "src"))
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("notes.md")
        x, y = at(s, "notes.md")
        ctrl_click(s, x, y)
        screen = editor_text(s)
        check(
            "a root-relative path is found from below the root",
            f"EDIT {d}/notes.md" in screen,
            screen,
        )


def test_an_osc8_file_link_is_taken_at_its_word():
    d = workdir()
    target = os.path.join(d, "src", "app.c")
    sh = [
        "/bin/sh",
        "-c",
        f"printf '\\033]8;;file://{target}\\033\\\\linked\\033]8;;\\033\\\\ x\\n';"
        " stty raw -echo; cat",
    ]
    c = conf(f'open_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("linked")
        x, y = at(s, "linked")
        ctrl_click(s, x + 2, y)
        screen = editor_text(s)
        check("the link's file, not the word", f"EDIT {target}" in screen, screen)


def test_a_link_to_another_host_is_not_ours():
    d = workdir()
    target = os.path.join(d, "src", "app.c")
    sh = [
        "/bin/sh",
        "-c",
        f"printf '\\033]8;;file://elsewhere.invalid{target}\\033\\\\linked"
        "\\033]8;;\\033\\\\ x\\n'; stty raw -echo; cat",
    ]
    c = conf(f'open_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("linked")
        x, y = at(s, "linked")
        ctrl_click(s, x, y)
        s.settle(40)
        check("nothing opened", len(s.panes()) == 1, str(s.panes()))


def test_a_wrapped_path_is_one_path():
    d = workdir()
    deep = os.path.join(d, "a" * 30, "b" * 30)
    os.makedirs(deep)
    open(os.path.join(deep, "file.c"), "w").close()
    rel = f"{'a' * 30}/{'b' * 30}/file.c"
    sh = shell_printing(rel, d)
    c = conf(f'open_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=40, rows=30, config=c) as s:
        s.until_text("file.c")
        x, y = at(s, "file.c")
        ctrl_click(s, x, y)
        screen = s.until_text("EDIT ").screen()
        # The second row alone (`bbb.../file.c`) is no file from `d`, so an
        # editor at all means the rows were joined back into one path.
        check("the path the width broke is opened whole", len(s.panes()) == 2, screen)


def test_over_a_mouse_tracking_program_it_takes_shift_too():
    """A modified click can mean something to a program that asked for the
    mouse, so there ctrl+click is the program's and shift+ctrl+click is
    slosh's -- the same escape a selection over such a program uses."""
    d = workdir()
    sh = [
        "/bin/sh",
        "-c",
        f"cd '{d}'; printf '\033[?1000h\033[?1006hnotes.md\n'; stty raw -echo; cat",
    ]
    c = conf(f'open_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("notes.md")
        x, y = at(s, "notes.md")
        ctrl_click(s, x, y)
        s.settle(40)
        check("ctrl alone is the program's", len(s.panes()) == 1, str(s.panes()))
        ctrl_click(s, x, y, shift=True)
        screen = editor_text(s)
        check("shift+ctrl is slosh's", f"EDIT {d}/notes.md" in screen, screen)


def test_open_in_float():
    d = workdir()
    sh = shell_printing("notes.md", d)
    c = conf(f'open_in "float"\nopen_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("notes.md")
        x, y = at(s, "notes.md")
        ctrl_click(s, x, y)
        editor_text(s)
        new = [p for p in s.panes() if p["id"] != 1]
        check("it floats", new and new[0]["floating"], str(s.panes()))


def test_open_in_tab():
    d = workdir()
    sh = shell_printing("notes.md", d)
    c = conf(f'open_in "tab"\nopen_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("notes.md")
        x, y = at(s, "notes.md")
        ctrl_click(s, x, y)
        editor_text(s)
        tabs = s.tabs()
        check(
            "a tab of its own, named for the file",
            len(tabs) == 2 and any(t.get("name") == "notes.md" for t in tabs),
            str(tabs),
        )


def test_the_editor_pane_goes_with_the_editor():
    d = workdir()
    sh = shell_printing("notes.md", d)
    c = conf(f'open_command "{EDITOR} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("notes.md")
        x, y = at(s, "notes.md")
        ctrl_click(s, x, y)
        editor_text(s)
        new = [p for p in s.panes() if p["id"] != 1][0]
        s.api("raw", id=new["id"], data="\r")
        s.until(lambda _: len(s.panes()) == 1)
        check("quitting it closes the pane", len(s.panes()) == 1, str(s.panes()))


def test_the_socket_verb():
    d = workdir()
    sh = shell_printing("hello", d)
    c = conf(f'open_command "{EDITOR} +{{line}} {{path}}"\n')
    with Session(sh, cols=100, rows=30, config=c) as s:
        s.until_text("hello")
        r = s.api("open", path="src/app.c:33")
        check("ok", r.get("ok") is True, str(r))
        check("resolved where the pane is", r.get("path") == f"{d}/src/app.c", str(r))
        check("with the line it carried", r.get("line") == 33, str(r))
        screen = editor_text(s)
        check("and opened", f"EDIT +33 {d}/src/app.c" in screen, screen)

        r = s.api("open", path="no/such/file.c")
        check("a missing file is refused", r.get("ok") is False, str(r))


def ctrl_hover(s, x, y, ctrl=True):
    """Pointer motion onto a cell, with or without ctrl held (SGR 32 = motion,
    16 = ctrl)."""
    s.send(f"\\e[<{48 if ctrl else 32};{x + 1};{y + 1}M")


def underlined(snap, y):
    return [
        (r["x"], r["w"])
        for r in snap.styles
        if r["y"] == y and "underline" in r["attrs"]
    ]


def status_row(snap):
    """The status line: the last non-blank row of the screen."""
    for y in range(snap.rows - 1, -1, -1):
        if snap.line(y).strip():
            return snap.line(y)
    return ""


def test_holding_ctrl_over_a_path_underlines_it_and_names_it():
    d = workdir()
    sh = shell_printing("x src/app.c:12:5: error", d)
    with Session(sh, cols=100, rows=20, config=conf()) as s:
        s.until_text("error")
        x, y = at(s, "app.c")
        ctrl_hover(s, x, y)
        snap = s.snapshot()
        sx, _ = at(s, "src/app.c")
        check(
            "the path and its numbers are underlined, the colon after is not",
            underlined(snap, y) == [(sx, len("src/app.c:12:5"))],
            str(underlined(snap, y)),
        )
        check(
            "the status line says what a click would open",
            f"open {d}/src/app.c:12:5" in status_row(snap),
            repr(status_row(snap)),
        )

        ctrl_hover(s, x, y, ctrl=False)
        snap = s.snapshot()
        check(
            "letting go of ctrl takes the underline away",
            underlined(snap, y) == [],
            str(underlined(snap, y)),
        )
        check("and the name", "open " not in status_row(snap), repr(status_row(snap)))


def test_no_indicator_over_what_a_click_would_not_open():
    d = workdir()
    sh = shell_printing("src/missing.c here", d)
    with Session(sh, cols=100, rows=20, config=conf()) as s:
        s.until_text("here")
        x, y = at(s, "missing")
        ctrl_hover(s, x, y)
        snap = s.snapshot()
        check("nothing underlined", underlined(snap, y) == [], str(underlined(snap, y)))
        check("nothing named", "open " not in status_row(snap), repr(status_row(snap)))


def test_a_wrapped_path_is_underlined_on_both_rows():
    d = workdir()
    deep = os.path.join(d, "a" * 30, "b" * 30)
    os.makedirs(deep)
    open(os.path.join(deep, "file.c"), "w").close()
    rel = f"{'a' * 30}/{'b' * 30}/file.c"
    with Session(shell_printing(rel, d), cols=40, rows=20, config=conf()) as s:
        s.until_text("file.c")
        x, y = at(s, "file.c")
        ctrl_hover(s, x, y)
        snap = s.snapshot()
        both = underlined(snap, y - 1) and underlined(snap, y)
        check(
            "both rows of it",
            bool(both),
            f"{underlined(snap, y - 1)} / {underlined(snap, y)}",
        )


def test_a_long_path_is_elided_in_the_middle():
    d = workdir()
    deep = os.path.join(d, "a-rather-long-directory", "and-another-one")
    os.makedirs(deep)
    open(os.path.join(deep, "target.c"), "w").close()
    sh = shell_printing("a-rather-long-directory/and-another-one/target.c", d)
    with Session(sh, cols=60, rows=20, config=conf()) as s:
        s.until_text("target.c")
        x, y = at(s, "target.c")
        ctrl_hover(s, x, y)
        row = status_row(s.snapshot())
        check("the file name survives", "target.c" in row, repr(row))
        check("the middle is what went", "\u2026" in row, repr(row))
        check("and it still fits beside the pane count", "pane 1/1" in row, repr(row))


def test_open_hover_picks_the_indicators():
    d = workdir()
    sh = shell_printing("notes.md", d)
    for mode, want_line, want_status in (
        ("off", False, False),
        ("underline", True, False),
        ("status", False, True),
    ):
        with Session(sh, cols=100, rows=20, config=conf(f'open_hover "{mode}"\n')) as s:
            s.until_text("notes.md")
            x, y = at(s, "notes.md")
            ctrl_hover(s, x, y)
            snap = s.snapshot()
            check(
                f"{mode}: underline {want_line}",
                bool(underlined(snap, y)) == want_line,
                str(underlined(snap, y)),
            )
            check(
                f"{mode}: status {want_status}",
                ("open " in status_row(snap)) == want_status,
                repr(status_row(snap)),
            )


for name, fn in sorted(list(globals().items())):
    if name.startswith("test_"):
        fn()
sys.exit(report())
