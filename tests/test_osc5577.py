#!/usr/bin/env python3
"""M4: OSC 5577 — pane status bars, action buttons, click reports.

Byte-compatible with the sl0ppi fork, so these are also the acceptance tests
for the existing pi extensions. The hostile cases matter as much as the happy
ones: a pane's own output is untrusted input.
"""

import sys
import time

from harness import Session, check, report


# A pane that prints sequences and then reads, so click reports are visible.
# Every % is doubled: these go through printf, which would otherwise eat %3B
# as a (broken) format specifier — which cost one confusing test failure.
def emit(*seqs):
    body = "".join('printf "%s";' % s.replace("%", "%%") for s in seqs)
    return ["/bin/sh", "-c", body + " stty raw -echo; cat -v"]


def shell():
    """A pane that executes lines we send it, without a line editor.

    Not an interactive shell: readline treats a typed ESC as a meta prefix and
    eats it, so `printf "<ESC>]5577;..."` never reached printf. A raw-mode read
    loop passes bytes through untouched.
    """
    return ["/bin/sh", "-c", 'stty raw -echo; while IFS= read -r l; do eval "$l"; done']


def emit_cmd(payload):
    """A shell line, typed at a pane, that prints one OSC 5577 sequence.

    The driver's own unescaper reads \\e as ESC (and \\0 as NUL, which is why
    writing \\033 here silently produced NUL + "33" and cost a debugging
    detour). So the ESC is handed over as \\e and the pane's printf sees a
    real one.
    """
    return 'printf "\\e]5577;%s\\x07"\\n' % payload.replace("%", "%%")


ST = "\\033\\\\"


def osc(payload):
    return "\\033]5577;" + payload + ST


def test_status_and_buttons():
    with Session(
        emit(
            osc("1;status;building 3/7"), osc("1;buttons;approve:Approve;cancel:Cancel")
        ),
        cols=62,
        rows=8,
    ) as s:
        s.settle()
        snap, p = s.snapshot(), s.pane()
        bottom = snap.line(p["y"] + p["h"] - 1)
        check(
            "status text is drawn in the frame", "building 3/7" in bottom, repr(bottom)
        )
        check(
            "buttons are drawn",
            "[Approve]" in bottom and "[Cancel]" in bottom,
            repr(bottom),
        )
        check(
            "buttons are anchored right of the status",
            bottom.index("building") < bottom.index("[Approve]"),
            repr(bottom),
        )
        check(
            "the frame is intact around it",
            bottom.startswith("  ╰") and bottom.rstrip().endswith("╯"),
            repr(bottom),
        )

        # nothing of the sequence itself may reach the screen
        check(
            "the escape sequence is not rendered",
            "5577" not in snap.screen(),
            repr(snap.screen()[:120]),
        )


def test_click_reports():
    with Session(
        emit(osc("1;buttons;approve:Approve;cancel:Cancel")), cols=62, rows=8
    ) as s:
        s.settle()
        snap, p = s.snapshot(), s.pane()
        pos = snap.find("[Approve]")
        check("the button is on screen", pos is not None, repr(snap.screen()))
        if not pos:
            return
        x, y = pos
        action = snap.hit_at(x + 1, y)
        check(
            "the click target is where the glyph is",
            action == f"btn:{p['id']}:approve",
            str(action),
        )
        check(
            "outside the button is not the button",
            snap.hit_at(x - 1, y) != f"btn:{p['id']}:approve",
            str(snap.hit_at(x - 1, y)),
        )

        s.click(x + 1, y)
        s.settle()
        out = s.snapshot().pane_text(p)
        check(
            "clicking sends a click report to the pane",
            "]5577;1;click;approve" in out,
            repr(out[:120]),
        )

        cancel = s.snapshot().find("[Cancel]")
        s.click(cancel[0] + 1, cancel[1])
        s.settle()
        out = s.snapshot().pane_text(p)
        check("the right button is reported", "click;cancel" in out, repr(out[:160]))


def test_replace_and_clear():
    with Session(shell(), cols=50, rows=8) as s:
        s.settle()
        p = s.pane()
        bottom = lambda: s.snapshot().line(p["y"] + p["h"] - 1)

        s.raw(emit_cmd("1;status;first"))
        s.raw(emit_cmd("1;buttons;a:A"))
        s.settle()
        check("initial status", "first" in bottom(), repr(bottom()))

        s.raw(emit_cmd("1;status;second"))
        s.settle()
        check(
            "status replaces, it does not append",
            "second" in bottom() and "first" not in bottom(),
            repr(bottom()),
        )
        check(
            "buttons are untouched by a status update",
            "[A]" in bottom(),
            repr(bottom()),
        )

        s.raw(emit_cmd("1;clear"))
        s.settle()
        check(
            "clear removes both",
            "second" not in bottom() and "[A]" not in bottom(),
            repr(bottom()),
        )


def test_escaping():
    with Session(
        emit(
            osc("1;status;a;b;c"), osc("1;buttons;go:one%3Btwo;colon:a%3Ab;pct:100%25")
        ),
        cols=70,
        rows=8,
    ) as s:
        s.settle()
        p = s.pane()
        bottom = s.snapshot().line(p["y"] + p["h"] - 1)
        check(
            "status keeps its semicolons (everything after status; is text)",
            "a;b;c" in bottom,
            repr(bottom),
        )
        check(
            "a %3B in a label becomes a semicolon", "[one;two]" in bottom, repr(bottom)
        )
        check("a %3A in a label becomes a colon", "[a:b]" in bottom, repr(bottom))
        check("a %25 in a label becomes a percent", "[100%]" in bottom, repr(bottom))


def test_hostile_input():
    """A pane's output is untrusted. None of this may take effect."""
    with Session(
        emit(
            osc("2;status;from the future"),
            osc("1;nonsense;whatever"),
            osc("1;buttons;bad id:Nope;:NoId;ok:"),
            osc("1;buttons;" + "x" * 40 + ":TooLong"),
            osc("1;status;legit"),
        ),
        cols=70,
        rows=8,
    ) as s:
        s.settle()
        p = s.pane()
        snap = s.snapshot()
        bottom = snap.line(p["y"] + p["h"] - 1)
        check(
            "a future version is ignored entirely",
            "from the future" not in bottom,
            repr(bottom),
        )
        check("an unknown verb is ignored", "whatever" not in bottom, repr(bottom))
        check("an invalid button id is dropped", "[Nope]" not in bottom, repr(bottom))
        check("an empty id is dropped", "[NoId]" not in bottom, repr(bottom))
        check(
            "an empty label is dropped",
            snap.hit_at(p["x"] + p["w"] - 3, p["y"] + p["h"] - 1) is None
            or "ok" not in str(snap.hits),
            str(snap.hits),
        )
        check("an over-long id is dropped", "[TooLong]" not in bottom, repr(bottom))
        check(
            "valid sequences after invalid ones still work",
            "legit" in bottom,
            repr(bottom),
        )

    # in-band purpose: allowed when free, refused when declared
    with Session(emit(osc("1;purpose;agent:main")), cols=50, rows=8) as s:
        s.settle()
        p = s.pane()
        check(
            "a pane may label itself when nothing is declared",
            p["purpose"] == "agent:main" and not p["purpose_declared"],
            str(p),
        )

    with Session(shell(), cols=50, rows=8) as s:
        # declare first, the way a layout or the sl0ppi CLI would
        pid = s.pane()["id"]
        s.api(
            "set-purpose", target="pane", id=pid, purpose="project:real", declared=True
        )
        s.settle()
        # `\e`, not `\033`: the driver's unescaper reads `\0` as NUL, so a `\033`
        # written here reached the pane as NUL + "33" and printf emitted no ESC
        # at all -- which made this check pass without ever testing anything.
        s.raw(emit_cmd("1;purpose;evil:relabelled"))
        s.settle()
        check(
            "a pane cannot relabel a declared purpose",
            s.pane()["purpose"] == "project:real",
            str(s.pane()),
        )
        # ...and the refusal is the lock, not the delivery: the same sequence
        # lands once the declared purpose is cleared.
        s.api("set-purpose", target="pane", id=pid, purpose="", declared=True)
        s.raw(emit_cmd("1;purpose;in-band:again"))
        s.settle()
        check(
            "and the sequence itself was arriving all along",
            s.pane()["purpose"] == "in-band:again",
            str(s.pane()),
        )


def test_split_across_reads():
    """A sequence arriving in pieces must still be understood."""
    with Session(shell(), cols=60, rows=8) as s:
        s.settle()
        p = s.pane()
        # three separate writes, settled in between, so the scanner really does
        # meet the sequence in pieces rather than all at once
        s.raw(r'printf "\e]5577;1;stat"' + "\\n")
        s.settle(80)
        s.raw(r'printf "us;in pieces"' + "\\n")
        s.settle(80)
        s.raw(r'printf "\x07"' + "\\n")
        s.settle()
        bottom = s.snapshot().line(p["y"] + p["h"] - 1)
        check(
            "a sequence split across reads is still understood",
            "in pieces" in bottom,
            repr(bottom),
        )


def test_pi_extension_compat():
    """The exact byte patterns sl0ppi's pi extensions emit (answer-picker).

    OSC = "\\x1b]5577;1;"   ST = "\\x1b\\\\"
    write(`${OSC}buttons;${spec.join(";")}${ST}`)
    write(`${OSC}buttons${ST}`)          // drop buttons, keep the status
    write(`${OSC}status;alt+N · ready${ST}`)
    CLICK_RE = /\\x1b\\]5577;1;click;([A-Za-z0-9_-]{1,32})(?:\\x1b\\\\|\\x07)/g
    escLabel: % -> %25, ; -> %3B, : -> %3A
    """
    with Session(
        emit(
            osc("1;buttons;ans-continue:continue;ans-yes:yes"),
            osc("1;status;alt+N · ready"),
        ),
        cols=70,
        rows=8,
    ) as s:
        s.settle()
        p, snap = s.pane(), s.snapshot()
        bottom = snap.line(p["y"] + p["h"] - 1)
        check(
            "answer-picker's buttons render",
            "[continue]" in bottom and "[yes]" in bottom,
            repr(bottom),
        )
        check("answer-picker's status renders", "alt+N · ready" in bottom, repr(bottom))

        x, y = snap.find("[continue]")
        s.click(x + 1, y)
        s.settle()
        out = s.snapshot().pane_text(p)
        # cat -v shows ESC as ^[ , so this is the extension's CLICK_RE on screen
        check(
            "the click report matches the extension's CLICK_RE",
            "^[]5577;1;click;ans-continue^[\\" in out,
            repr(out[:140]),
        )

    with Session(shell(), cols=70, rows=8) as s:
        s.settle()
        p = s.pane()
        bottom = lambda: s.snapshot().line(p["y"] + p["h"] - 1)
        s.raw(emit_cmd("1;status;alt+N · ready"))
        s.raw(emit_cmd("1;buttons;ans-yes:yes"))
        s.settle()
        check(
            "status and buttons together",
            "[yes]" in bottom() and "ready" in bottom(),
            repr(bottom()),
        )

        s.raw(emit_cmd("1;buttons"))  # hideButtons(): verb with no payload
        s.settle()
        check(
            "`buttons` with no payload drops only the buttons",
            "[yes]" not in bottom() and "ready" in bottom(),
            repr(bottom()),
        )


def test_hello_handshake():
    with Session(emit(osc("1;hello;")), cols=60, rows=8) as s:
        s.settle()
        out = s.snapshot().pane_text(s.pane())
        check(
            "hello is answered with an implementation and a version",
            "5577;1;hello-reply;slosh;1" in out,
            repr(out[:120]),
        )


def test_a_reply_is_not_a_request():
    """The pane in `emit()` runs `cat -v`, so everything the session sends it
    goes straight back out again. That is the ordinary case -- a shell with echo
    on, a REPL waiting for a line -- and it used to be a loop: `hello` was
    answered with the verb `hello`, which parsed as another request. A pane
    running `tee` traded four megabytes with the session in a second and a half.

    So: exactly one answer, however loudly the pane echoes it.
    """
    with Session(emit(osc("1;hello;")), cols=60, rows=12) as s:
        s.settle()
        time.sleep(0.5)  # real time: a loop needs none of our help to run
        out = s.snapshot().pane_text(s.pane())
        # The one answer can appear on screen twice: `cat -v` prints it, and if
        # it arrived before the pane's `stty -echo` took effect, ECHOCTL echoed
        # it too, as the same printable `^[`. Both are one reply, displayed by
        # the pane's own doing; a loop is hundreds. Count on de-wrapped text --
        # at 60 columns the second copy can straddle a line break, which is how
        # a seven-character implementation name hid it from this count and made
        # `== 1` pass for the wrong reason.
        n = out.replace("\n", "").count("hello-reply")
        check(
            "a reply the pane echoes back is not answered again",
            1 <= n <= 2,
            "%d copies: %r" % (n, out[:200]),
        )
        check(
            "and the pane is still usable afterwards",
            len(out) < 400,
            "%d chars of screen" % len(out),
        )


if __name__ == "__main__":
    test_status_and_buttons()
    test_click_reports()
    test_replace_and_clear()
    test_escaping()
    test_hostile_input()
    test_split_across_reads()
    test_pi_extension_compat()
    test_hello_handshake()
    test_a_reply_is_not_a_request()
    sys.exit(report())
