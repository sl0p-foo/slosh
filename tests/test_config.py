#!/usr/bin/env python3
"""M6: the config file (D2).

Everything here was a compiled-in constant until now. The important property
is not that settings apply — it is that a broken config costs a warning and
never a terminal.
"""
import os
import subprocess
import sys
import tempfile
import time

from harness import Session, check, report, BIN

SH = ["/bin/sh", "-c", 'printf "\\033]2;cfg\\007"; stty raw -echo; cat']


def hover(s, x, y):
    s.send(rf"\e[<35;{x + 1};{y + 1}M")


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def test_geometry():
    path = cfg("""
        gap 2
        gap_aspect 1
        rounded false
        title_align "left"
    """)
    with Session(SH, cols=60, rows=14, config=path) as s:
        s.settle()
        snap, p = s.snapshot(), s.pane()
        check("gap is applied", p["x"] == 2 and p["y"] == 3, str(p))
        row = snap.line(p["y"])
        check("rounded false gives square corners",
              "┌" in row and "╭" not in row, repr(row))
        check("title_align left puts the title at the left",
              row.index("cfg") < len(row) // 3, repr(row))
    os.unlink(path)


def test_status_bar_off():
    path = cfg("status_bar false\n")
    with Session(SH, cols=50, rows=10, config=path) as s:
        s.settle()
        snap, p = s.snapshot(), s.pane()
        check("no status bar means no strip row", "panes" not in snap.screen(),
              repr(snap.line(1)))
        check("and the panes get the row back", p["y"] == 1, str(p))
    os.unlink(path)


def test_theme():
    path = cfg("""
        theme {
            frame_focus "#00ff00"
            frame_idle "#333333"
        }
    """)
    with Session(SH, cols=76, rows=14, config=path) as s:
        s.settle()
        p = s.pane()
        run = s.snapshot().style_at(p["x"], p["y"])
        check("the focused frame takes the configured colour",
              run and run["fg"] == "#00ff00", str(run))

        s.key("\\\\")
        s.settle()
        panes = s.panes()
        snap = s.snapshot()
        idle = [q for q in panes if not q["focused"]][0]
        run = snap.style_at(idle["x"], idle["y"])
        check("an unfocused frame takes the other one",
              run and run["fg"] == "#333333", str(run))
    os.unlink(path)


def test_keys():
    path = cfg("""
        keys {
            prefix "ctrl+b"
            bind "v" "split-cols"
            bind "\\\\" "none"
        }
    """)
    with Session(SH, cols=80, rows=16, config=path) as s:
        s.settle()
        s.send(r"\x02v")  # C-b v
        s.settle()
        check("a custom prefix and binding work", len(s.panes()) == 2,
              str(len(s.panes())))

        s.send("\\x02\\\\")  # C-b \  -> unbound now
        s.settle()
        check("a binding set to none does nothing", len(s.panes()) == 2,
              str(len(s.panes())))

        s.send(r"\x01")  # the old prefix is just a keystroke now
        s.settle()
        check("the old prefix is passed through to the pane",
              len(s.panes()) == 2 and s.api("alive")["alive"])

        s.send(r"\x02\x02")  # prefix twice is still a literal prefix
        s.settle()
        check("prefix twice sends the prefix itself", s.api("alive")["alive"])
    os.unlink(path)


def test_a_chord_can_be_written_the_way_the_cheatsheet_prints_it():
    """The sheet's whole claim is that what it shows is what you would write, and
    `--dump-config` wrote in that notation for months while the parser refused
    it. Both spellings are accepted now: `C-a` and `ctrl+a`, `S-tab` and
    `shift+tab`, `?` and `shift+slash`, `H` and `shift+h`, `←` and `left`."""
    path = cfg("""
        keys {
            prefix "C-b"
            bind "?" "split-cols"
            bind "S-tab" "split-rows"
            bind "→" "close-pane"
        }
    """)
    # Tall and wide enough for a column split and then a row split inside it:
    # min_split is a real floor and a refused split would look like a refused
    # binding.
    with Session(SH, cols=100, rows=30, config=path) as s:
        s.settle()
        # `?` is shift+slash, which is what the key says on the keyboard.
        s.send(r"\x02?")
        s.settle()
        check("`?` binds the key you press", len(s.panes()) == 2,
              str(len(s.panes())))

        s.send(r"\x02\e[Z")   # C-b shift+tab
        s.settle()
        check("`S-tab` is shift+tab", len(s.panes()) == 3, str(len(s.panes())))

        s.send(r"\x02\e[C")   # C-b right arrow
        s.settle()
        check("an arrow glyph is that arrow", len(s.panes()) == 2,
              str(len(s.panes())))
    os.unlink(path)

    # A capital letter is that letter with shift, not that letter: the sheet
    # prints shift+h as "H" for exactly the reason a config should be able to.
    path = cfg('keys {\n    bind "H" "split-cols"\n}\n')
    with Session(SH, cols=100, rows=30, config=path) as s:
        s.settle()
        s.send(r"\x01h")       # plain h still moves focus, it does not split
        s.settle()
        check("a plain letter is left alone", len(s.panes()) == 1,
              str(len(s.panes())))
        s.send(r"\x01H")
        s.settle()
        check("and the capital is the shifted one", len(s.panes()) == 2,
              str(len(s.panes())))
    os.unlink(path)


def test_min_pane_drives_collapse():
    # Split over the control API rather than the keyboard. This is about what
    # the *layout* does with a split that does not fit, and the keyboard now
    # declines to make one — which is a different rule, tested elsewhere. A
    # script asking for a pane is declaring what it wants, so it still gets it.
    roomy = cfg("min_pane cols=10 rows=3\n")
    tight = cfg("min_pane cols=40 rows=12\n")
    with Session(SH, cols=70, rows=16, config=roomy) as s:
        s.settle()
        s.api("split", dir="cols")
        s.settle()
        check("a low floor keeps both panes visible",
              not any(p["hidden"] for p in s.panes()), str(s.panes()))
    with Session(SH, cols=70, rows=16, config=tight) as s:
        s.settle()
        s.api("split", dir="cols")
        s.settle()
        check("a high floor collapses the same split",
              any(p["hidden"] for p in s.panes()), str(s.panes()))
    os.unlink(roomy)
    os.unlink(tight)


def test_fail_open():
    """A broken config must cost a warning, never a terminal."""
    broken = cfg('gap 1\ntheme {\n  frame_focus "#00ff00"\n')  # unclosed block
    with Session(SH, cols=60, rows=14, config=broken) as s:
        s.settle()
        check("a session still starts on a broken config",
              s.api("alive")["alive"], "")
        p = s.pane()
        check("and uses the compiled-in defaults", p["x"] == 2 and p["y"] == 2,
              str(p))
    proc = subprocess.run([BIN, "--headless", "--cols", "20", "--rows", "3",
                           "--", "/bin/echo", "hi"],
                          capture_output=True, text=True,
                          env=dict(os.environ, SL0PPTY_CONFIG=broken))
    check("the reason is reported on stderr",
          "line" in proc.stderr or "parse" in proc.stderr, repr(proc.stderr))
    check("but the run succeeds", proc.returncode == 0, proc.stderr)
    os.unlink(broken)

    partial = cfg("""
        gap 3
        keys { bind "v" "no-such-action" }
    """)
    with Session(SH, cols=60, rows=14, config=partial) as s:
        s.settle()
        # gap 3, aspect 2 (default) -> 6 columns of margin
        check("a bad binding does not discard the rest of the file",
              s.pane()["x"] == 6, str(s.pane()))
    os.unlink(partial)

    missing = "/nonexistent/definitely/not/here.kdl"
    proc = subprocess.run([BIN, "--headless", "--cols", "20", "--rows", "3",
                           "--", "/bin/echo", "hi"],
                          capture_output=True, text=True,
                          env=dict(os.environ, SL0PPTY_CONFIG=missing))
    check("a missing config is silent", proc.stderr == "", repr(proc.stderr))


def test_reload():
    path = cfg("gap 1\n")
    with Session(SH, cols=60, rows=14, config=path) as s:
        s.settle()
        check("before reload", s.pane()["x"] == 2, str(s.pane()))
        with open(path, "w") as f:
            f.write("gap 3\n")
        r = s.api("reload")
        s.settle()
        check("reload applies the new file", r["ok"] and s.pane()["x"] == 6,
              f"{r} {s.pane()}")

        with open(path, "w") as f:
            f.write("gap 5\ntheme {\n")  # broken
        r = s.api("reload")
        s.settle()
        check("a broken reload is refused", not r["ok"], str(r))
        check("and the working config is kept", s.pane()["x"] == 6,
              str(s.pane()))
    os.unlink(path)


def test_status_line_reserves_a_row_at_the_bottom():
    on = cfg("")
    off = cfg("status_line false\n")
    heights = {}
    for name, path in (("on", on), ("off", off)):
        with Session(SH, cols=50, rows=12, config=path) as s:
            s.settle()
            heights[name] = s.pane()["h"]
    check("turning the status line off gives the row back to the panes",
          heights["off"] == heights["on"] + 1, str(heights))
    os.unlink(on)
    os.unlink(off)


def test_status_line_says_what_you_are_looking_at():
    lay = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    lay.write('layout {\n tab name="api" purpose="project:api.a1" {\n'
              '  pane purpose="agent:main"\n'
              '  pane suspended=true command="top"\n }\n}\n')
    lay.close()
    with Session(SH, cols=90, rows=18, layout=lay.name) as s:
        s.settle(20)
        bottom = s.snapshot().text[-2]
        check("it names the tab and the focused pane",
              "api" in bottom and "agent:main" in bottom, repr(bottom))
        # The bottom carries a count too, but it is this tab's rather than the
        # session's (see test_the_two_pane_counts_answer_different_questions).
        # What it must not do is restate the strip itself.
        check("and is not a second copy of the tab strip",
              "pane 1/2" in bottom and "1:api" not in bottom, repr(bottom))

        ids = [p["id"] for p in s.panes()]
        s.api("focus", id=ids[1])
        s.settle(20)
        check("a pane that has not started says so",
              "not started" in s.snapshot().text[-2],
              repr(s.snapshot().text[-2]))
    os.unlink(lay.name)


def test_status_line_reports_scrollback():
    noisy = ["/bin/sh", "-c",
             'printf "\\033]2;p\\007"; i=0; while [ $i -lt 200 ]; do '
             'echo "line $i"; i=$((i+1)); done; stty raw -echo; cat']
    with Session(noisy, cols=90, rows=18) as s:
        s.settle(30)
        check("nothing is said while you are in the present",
              "scrolled" not in s.snapshot().text[-2],
              repr(s.snapshot().text[-2]))
        p = s.pane()
        for _ in range(6):
            s.send(rf"\e[<64;{p['content_x'] + 2};{p['content_y'] + 2}M")
        s.settle(30)
        check("looking at the past is said out loud",
              "scrolled" in s.snapshot().text[-2],
              repr(s.snapshot().text[-2]))


def test_the_two_pane_counts_answer_different_questions():
    """The strip above counts the session. The line below says which of this
    tab's panes you are in, which is the question you actually have once a tab
    has collapsed into a list and only one of them is open."""
    lay = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    lay.write('layout {\n tab name="api" {\n  pane\n  pane\n }\n'
              ' tab name="notes" {\n  pane\n }\n}\n')
    lay.close()
    with Session(SH, cols=80, rows=16, layout=lay.name) as s:
        s.settle(20)
        top, bottom = s.snapshot().line(1), s.snapshot().text[-2]
        check("the strip counts every pane in the session",
              "3 panes" in top, repr(top))
        check("the line says which of this tab's panes is open",
              "pane 1/2" in bottom, repr(bottom))

        ids = [p["id"] for p in s.panes() if p["tab"] == 1]
        s.api("focus", id=ids[1])
        s.settle(20)
        check("the index moves with the focus",
              "pane 2/2" in s.snapshot().text[-2],
              repr(s.snapshot().text[-2]))
        check("while the session count does not",
              "3 panes" in s.snapshot().line(1), repr(s.snapshot().line(1)))

        tabs = s.tabs()
        s.api("select-tab", id=tabs[1]["id"])
        s.settle(20)
        top, bottom = s.snapshot().line(1), s.snapshot().text[-2]
        check("a one-pane tab says so", "pane 1/1" in bottom, repr(bottom))
        check("and the session count still counts the session",
              "3 panes" in top, repr(top))

        s.api("split", dir="cols")
        s.settle(20)
        check("splitting moves both numbers, each in its own way",
              "pane 2/2" in s.snapshot().text[-2]
              and "4 panes" in s.snapshot().line(1),
              repr(s.snapshot().text[-2]) + " / " + repr(s.snapshot().line(1)))
    os.unlink(lay.name)


def test_status_pad_holds_both_bars_off_the_edge():
    lay = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    lay.write('layout {\n tab name="api" {\n  pane\n }\n}\n')
    lay.close()
    seen = {}
    for pad in (2, 8):
        path = cfg(f"status_pad {pad}\n")
        with Session(SH, cols=80, rows=16, config=path, layout=lay.name) as s:
            s.settle(20)
            top, bottom = s.snapshot().line(1), s.snapshot().text[-2]
            seen[pad] = (len(top) - len(top.lstrip()),
                         len(top.rstrip()),
                         len(bottom) - len(bottom.lstrip()),
                         len(bottom.rstrip()))
        os.unlink(path)
    check("a bigger pad indents the strip further from the left",
          seen[8][0] == seen[2][0] + 6, str(seen))
    check("and holds it further off the right",
          seen[8][1] == seen[2][1] - 6, str(seen))
    check("the line below is padded the same on the left",
          seen[8][2] == seen[2][2] + 6, str(seen))
    check("and on the right", seen[8][3] == seen[2][3] - 6, str(seen))
    os.unlink(lay.name)



def test_every_surface_has_its_own_theme_name():
    """The point of splitting six names into thirty-five: recolouring one
    surface must not recolour another that happened to share an entry."""
    conf = cfg('theme {\n'
               '  guide "#00ff00"\n'
               '  tab_active_bg "#0000ff"\n'
               '  status "#ff8800"\n'
               '  header "#ff00ff"\n'
               '}\n')
    with Session(SH, cols=80, rows=16, config=conf) as s:
        s.settle(20)
        snap = s.snapshot()

        tab = [h for h in snap.hits if h["action"].startswith("tab:")][0]
        check("tab_active_bg fills the tab you are in",
              (snap.style_at(tab["x"] + 1, tab["y"]) or {}).get("bg") == "#0000ff",
              str(snap.style_at(tab["x"] + 1, tab["y"])))
        check("status colours the bottom line",
              (snap.style_at(4, snap.rows - 2) or {}).get("fg") == "#ff8800",
              str(snap.style_at(4, snap.rows - 2)))

        # ...and the frame, which shares a *default* with guide, did not move.
        p = s.pane()
        check("the focused frame keeps its own colour",
              (snap.style_at(p["x"], p["y"] + 1) or {}).get("fg") == "#ff5fd7",
              str(snap.style_at(p["x"], p["y"] + 1)))

        # The guide is the one that used to be the same entry as the frame.
        hover(s, p["x"], p["y"] + 3)
        time.sleep(0.35)
        s.settle(40)
        snap = s.snapshot()
        check("and the split guide takes the colour named for it",
              (snap.style_at(p["x"], p["y"] + 3) or {}).get("fg") == "#00ff00",
              str(snap.style_at(p["x"], p["y"] + 3)))
    os.unlink(conf)


def test_an_unknown_theme_name_is_refused_not_ignored():
    conf = cfg('theme {\n  frame_focus "not-a-colour"\n}\n')
    with Session(SH, cols=60, rows=12, config=conf) as s:
        s.settle(20)
        check("a bad colour costs a warning, not a terminal", s.alive(), "")
        p = s.pane()
        check("and the compiled-in default is kept",
              (s.snapshot().style_at(p["x"], p["y"] + 1) or {}).get("fg")
              == "#ff5fd7",
              str(s.snapshot().style_at(p["x"], p["y"] + 1)))
    os.unlink(conf)


if __name__ == "__main__":
    test_geometry()
    test_status_bar_off()
    test_theme()
    test_keys()
    test_min_pane_drives_collapse()
    test_fail_open()
    test_reload()
    test_status_line_reserves_a_row_at_the_bottom()
    test_status_line_says_what_you_are_looking_at()
    test_status_line_reports_scrollback()
    test_the_two_pane_counts_answer_different_questions()
    test_status_pad_holds_both_bars_off_the_edge()
    test_every_surface_has_its_own_theme_name()
    test_an_unknown_theme_name_is_refused_not_ignored()
    sys.exit(report())
