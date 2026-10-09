#!/usr/bin/env python3
"""OSC 7501 — the program status protocol, as a pane's status line.

The sequence a program sends is somebody else's specification
(https://www.superlogical.com/rex/docs/build/program-status, rev 0.2), which
makes this file the place where our reading of it is written down: what a
record is, which one gets the one line a pane has, when a record dies without
anybody saying so, and what a hostile report may not do.

The companion to test_osc5577.py, not a replacement for it: the two protocols
share the status slot and the tests next door own the other half of it.
"""

import base64
import sys
import tempfile
import time

from harness import Session, check, report


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def b64(text):
    return base64.b64encode(text.encode()).decode()


def shell():
    """A pane that evaluates lines we send it, without a line editor.

    Same reason as test_osc5577.py: readline eats a typed ESC as a meta
    prefix, so `printf "<ESC>]7501;..."` never reaches printf.
    """
    return ["/bin/sh", "-c", 'stty raw -echo; while IFS= read -r l; do eval "$l"; done']


def cmd(body):
    r"""A shell line, typed at a pane, that prints one OSC sequence.

    The driver's unescaper reads `\e` as ESC and `\0` as NUL — so `\033` here
    would arrive as NUL + "33" and printf would emit no ESC at all, which is
    how a RIS test once passed without ever resetting anything.
    """
    return 'printf "\\e]%s\\e\\\\\\\\"' % body.replace("%", "%%") + "\\n"


def emit(*seqs):
    """A pane that prints sequences and then echoes everything it is sent."""
    body = "".join('printf "%s";' % s.replace("%", "%%") for s in seqs)
    return ["/bin/sh", "-c", body + " stty raw -echo; cat -v"]


FIELDS = ("status", "busy", "program_state", "program_kind", "program_app", "progress")


def state(s):
    return {k: s.panes()[0][k] for k in FIELDS}


def test_a_report_becomes_the_pane_status():
    with Session(shell(), cols=70, rows=8) as s:
        s.settle()
        p = s.pane()
        bottom = lambda: s.snapshot().line(p["y"] + p["h"] - 1)

        s.raw(cmd("7501;state=working:app=cargo:progress=40:msg=" + b64("building")))
        s.settle()
        check(
            "the message is drawn in the frame", "building" in bottom(), repr(bottom())
        )
        check("progress rides along with it", "40%" in bottom(), repr(bottom()))
        check(
            "the sequence itself reaches no screen",
            "7501" not in s.snapshot().screen(),
            repr(s.snapshot().screen()[:120]),
        )

        st = state(s)
        check("`working` is busy", st["busy"], str(st))
        check("the state is reported", st["program_state"] == "working", str(st))
        check("so is the program's own name", st["program_app"] == "cargo", str(st))
        check("and the progress", st["progress"] == 40, str(st))
        check(
            "`status` is the one answer either protocol gives",
            st["status"] == "building 40%",
            str(st),
        )


def test_the_five_states():
    """Only two of them are something still happening. `done` and `error`
    describe work that stopped, and a spinner over either is a lie."""
    with Session(shell(), cols=70, rows=8) as s:
        s.settle()
        for name, busy in (
            ("idle", False),
            ("working", True),
            ("blocked", True),
            ("done", False),
            ("error", False),
        ):
            s.raw(cmd("7501;state=%s:msg=%s" % (name, b64("the " + name + " one"))))
            s.settle()
            st = state(s)
            check(
                "`%s` is %sbusy" % (name, "" if busy else "not "),
                st["busy"] == busy and st["program_state"] == name,
                str(st),
            )

        s.raw(cmd("7501;state=blocked:kind=question:msg=" + b64("Which branch?")))
        s.settle()
        check(
            "`kind` says what it is blocked on",
            state(s)["program_kind"] == "question",
            str(state(s)),
        )

        # kind means nothing anywhere else, and must not survive into a state
        # that cannot carry it
        s.raw(cmd("7501;state=working:kind=question:msg=" + b64("on")))
        s.settle()
        check(
            "`kind` is ignored outside `blocked`",
            state(s)["program_kind"] == "",
            str(state(s)),
        )

        s.raw(cmd("7501;state=done:progress=50:msg=" + b64("finished")))
        s.settle()
        check(
            "`progress` is ignored outside `working`/`blocked`",
            state(s)["progress"] == -1,
            str(state(s)),
        )


def test_a_report_replaces_its_record_whole():
    """A key the report does not carry is a key the record no longer has --
    which is why the spec tells programs to repeat `app` every time."""
    with Session(shell(), cols=70, rows=8) as s:
        s.settle()
        s.raw(cmd("7501;state=working:app=brew:msg=" + b64("installing")))
        s.settle()
        check("app arrived", state(s)["program_app"] == "brew", str(state(s)))

        s.raw(cmd("7501;state=working:msg=" + b64("still installing")))
        s.settle()
        st = state(s)
        check(
            "a report without app leaves none behind", st["program_app"] == "", str(st)
        )
        check(
            "...and the new message is the one shown",
            st["status"] == "still installing",
            str(st),
        )


def test_several_records():
    with Session(shell(), cols=78, rows=8) as s:
        s.settle()
        p = s.pane()
        bottom = lambda: s.snapshot().line(p["y"] + p["h"] - 1)

        s.raw(cmd("7501;state=working:app=deploy:msg=" + b64("Deploying v2.4.1")))
        s.raw(
            cmd(
                "7501;state=working:id=us-east:title=%s:progress=40:msg=%s"
                % (b64("US East"), b64("Pushing image"))
            )
        )
        s.settle()
        check(
            "a child record is shown with its title",
            "US East: Pushing image" in bottom(),
            repr(bottom()),
        )

        s.raw(
            cmd(
                "7501;state=blocked:kind=permission:id=eu-west:title=%s:msg=%s"
                % (b64("EU West"), b64("Approve deploy?"))
            )
        )
        s.settle()
        st = state(s)
        check(
            "the record waiting on a person wins the line",
            st["program_state"] == "blocked" and "EU West" in st["status"],
            str(st),
        )
        check(
            "a record with no app of its own inherits one",
            st["program_app"] == "deploy",
            str(st),
        )

        s.raw(cmd("7501;state=clear:id=eu-west"))
        s.settle()
        check(
            "clearing one record falls back to another, not to nothing",
            state(s)["program_state"] == "working" and "US East" in state(s)["status"],
            str(state(s)),
        )

        s.raw(cmd("7501;state=clear"))
        s.settle()
        st = state(s)
        check(
            "`clear` with no id removes every record",
            not st["status"] and not st["busy"],
            str(st),
        )
        check("...including the state", st["program_state"] == "", str(st))


def test_clear_takes_the_children_with_it():
    with Session(shell(), cols=70, rows=8) as s:
        s.settle()
        s.raw(cmd("7501;state=done:id=build:msg=" + b64("built")))
        s.raw(cmd("7501;state=blocked:id=build/test:msg=" + b64("Run the slow suite?")))
        s.settle()
        check(
            "the child is the one shown",
            "slow suite" in state(s)["status"],
            str(state(s)),
        )

        s.raw(cmd("7501;state=clear:id=build"))
        s.settle()
        st = state(s)
        check(
            "clearing a parent clears what is under it",
            st["program_state"] == "" and not st["status"],
            str(st),
        )


def test_records_die_with_the_prompt_and_the_program():
    """There is no heartbeat in this protocol: a `working` record lives until
    the terminal itself sees the work stop. A new shell prompt (OSC 133 A) and
    the program exiting are both that moment; `done` survives both, because
    being told what finished while you were away is the whole point."""
    with Session(shell(), cols=70, rows=8) as s:
        s.settle()
        s.raw(cmd("7501;state=working:msg=" + b64("compiling")))
        s.settle()
        check(
            "working, while it runs",
            state(s)["program_state"] == "working",
            str(state(s)),
        )

        s.raw(cmd("133;A"))
        s.settle()
        st = state(s)
        check(
            "a new prompt drops a `working` record", st["program_state"] == "", str(st)
        )
        check(
            "...and the line it composed", not st["status"] and not st["busy"], str(st)
        )

        s.raw(cmd("7501;state=done:msg=" + b64("built 3 targets")))
        s.raw(cmd("133;A"))
        s.settle()
        check(
            "`done` outlives the prompt",
            state(s)["program_state"] == "done" and "3 targets" in state(s)["status"],
            str(state(s)),
        )

        # the parameters a real shell integration adds must not hide the A
        s.raw(cmd("7501;state=working:msg=" + b64("and again")))
        s.raw(cmd("133;A;aid=7;cl=m"))
        s.settle()
        check(
            "a prompt mark with parameters still counts",
            state(s)["program_state"] == "",
            str(state(s)),
        )

        s.raw(cmd("7501;state=working:msg=" + b64("again")))
        s.raw('printf "\\ec"' + "\\n")  # RIS
        s.settle()
        check(
            "a full reset removes every record",
            state(s)["program_state"] == "",
            str(state(s)),
        )

    keep = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    keep.write('keep_dead "all"\n')  # so there is still a pane to ask
    keep.close()
    body = 'printf "\\033]7501;state=working:msg=%s\\033\\\\"' % b64("halfway")
    with Session(["/bin/sh", "-c", body], cols=70, rows=8, config=keep.name) as s:
        s.until(lambda snap: s.panes() and not s.panes()[0]["alive"])
        st = state(s)
        check(
            "a `working` record does not outlive its program",
            st["program_state"] == "" and not st["status"],
            str(st),
        )

    body = 'printf "\\033]7501;state=done:msg=%s\\033\\\\"' % b64("all done")
    with Session(["/bin/sh", "-c", body], cols=70, rows=8, config=keep.name) as s:
        s.until(lambda snap: s.panes() and not s.panes()[0]["alive"])
        st = state(s)
        check(
            "a `done` record does, so the result is still there to find",
            st["program_state"] == "done" and "all done" in st["status"],
            str(st),
        )
        check("...and nothing about it is still happening", not st["busy"], str(st))


def test_hostile_and_malformed_reports():
    """A pane's output is untrusted input. The spec's rule is that a report
    which breaks a limit is discarded *whole*, so none of this may leave a
    record half-written over the one that was there."""
    with Session(shell(), cols=70, rows=8) as s:
        s.settle()
        s.raw(cmd("7501;state=working:app=real:msg=" + b64("the real one")))
        s.settle()
        good = state(s)

        cases = (
            ("no state at all", "7501;msg=" + b64("nope")),
            ("an unknown state", "7501;state=exploded:msg=" + b64("nope")),
            ("base64 that is not base64", "7501;state=working:msg=not..base64"),
            (
                "a control character in the text",
                "7501;state=working:msg=" + base64.b64encode(b"two\nlines").decode(),
            ),
            # "escape code safe UTF-8": well-formed, and no C0/DEL/C1 --
            # checked on codepoints, so a C1 written as `C2 9B` is caught,
            # and text that leaves the terminal as JSON cannot be malformed
            (
                "a C1 control, written in UTF-8",
                "7501;state=working:msg="
                + base64.b64encode("a\u009bb".encode()).decode(),
            ),
            (
                "a bare 0x9b, which is not UTF-8 at all",
                "7501;state=working:msg=" + base64.b64encode(b"a\x9bb").decode(),
            ),
            (
                "a truncated UTF-8 sequence",
                "7501;state=working:msg=" + base64.b64encode(b"caf\xc3").decode(),
            ),
            (
                "an overlong encoding of /",
                "7501;state=working:msg=" + base64.b64encode(b"a\xc0\xafb").decode(),
            ),
            (
                "a lone surrogate",
                "7501;state=working:msg="
                + base64.b64encode(b"a\xed\xa0\x80b").decode(),
            ),
            (
                "an escape sequence in the text",
                "7501;state=working:msg="
                + base64.b64encode(b"\x1b]5577;1;status;owned\x1b\\").decode(),
            ),
            ("a message over the limit", "7501;state=working:msg=" + b64("x" * 2100)),
            ("a title over the limit", "7501;state=working:title=" + b64("x" * 300)),
            ("an over-long key", "7501;state=working:keylongerthansixteen=1"),
            ("an over-long app", "7501;state=working:app=" + "a" * 40),
            ("an empty id segment", "7501;state=working:id=a//b"),
            ("an id segment over 32", "7501;state=working:id=" + "a" * 40),
            ("an id deeper than 8", "7501;state=working:id=" + "/".join("abcdefghi")),
        )
        for why, body in cases:
            s.raw(cmd(body))
            s.settle()
            check(
                "%s is discarded whole" % why,
                state(s) == good,
                "%s -> %s" % (why, state(s)),
            )
            # ...and put the good record back, so one case that *does* land
            # cannot fail every case after it (which is how a 15-character
            # "over-long" key once produced five failures)
            s.raw(cmd("7501;state=working:app=real:msg=" + b64("the real one")))
            s.settle()

        # ...while a malformed *pair* is only skipped, and an unknown key is
        # how the protocol is extended: both leave the rest of the report.
        s.raw(cmd("7501;state=done:nonsense:=5:future=42:msg=" + b64("still read")))
        s.settle()
        st = state(s)
        check(
            "a malformed pair is skipped, the rest of the report is not",
            st["program_state"] == "done" and st["status"] == "still read",
            str(st),
        )

        s.raw(cmd("7501;state=working:progress=101:msg=" + b64("off the scale")))
        s.settle()
        check(
            "a progress outside 0..100 is unknown, not clamped",
            state(s)["progress"] == -1,
            str(state(s)),
        )

        s.raw(cmd("7501;state=working:kind=sudo:msg=" + b64("hm")))
        s.settle()
        check(
            "an unrecognised kind is absent",
            state(s)["program_kind"] == "",
            str(state(s)),
        )

        # 64 records is our cap, and a program that invents ids must not be
        # able to push out the ones that matter by reporting past it.
        for i in range(80):
            s.raw(cmd("7501;state=idle:id=junk%d" % i))
        s.raw(cmd("7501;state=blocked:id=real:msg=" + b64("still here")))
        s.settle()
        check(
            "the newest record survives a flood of invented ones",
            "still here" in state(s)["status"],
            str(state(s)),
        )


def test_a_long_message_is_shortened_not_broken():
    """A report may carry 2048 bytes of text; a pane status line has room for
    256 of them, and "terminals MAY shorten it". Shortening is allowed to lose
    words and not allowed to lose half a character -- the cut lands on a UTF-8
    boundary.

    This is also where the only real bug in the first draft lived: the boundary
    walk ran even when nothing was being cut, read one byte past the decoded
    message, and took the last character off every status in the file."""
    with Session(shell(), cols=70, rows=8) as s:
        s.settle()

        s.raw(cmd("7501;state=working:msg=" + b64("exactly this")))
        s.settle()
        check(
            "a message that fits is not touched",
            state(s)["status"] == "exactly this",
            repr(state(s)["status"]),
        )

        s.raw(cmd("7501;state=working:msg=" + b64("caf\u00e9 \u2588 \U0001f600")))
        s.settle()
        check(
            "multi-byte text survives the round trip",
            state(s)["status"] == "caf\u00e9 \u2588 \U0001f600",
            repr(state(s)["status"]),
        )

        # 400 three-byte characters: 1200 bytes, inside the protocol's limit
        # and well outside ours, with no cut point that is also a byte
        long = "\u2588" * 400
        s.raw(cmd("7501;state=working:msg=" + b64(long)))
        s.settle()
        got = state(s)["status"]
        check(
            "an over-long message is kept, shortened",
            got.startswith("\u2588\u2588"),
            repr(got[:20]),
        )
        check(
            "...to what the line holds", 0 < len(got) <= 256, "%d characters" % len(got)
        )
        check(
            "...and not through the middle of a character",
            set(got) == {"\u2588"},
            repr(got[-8:]),
        )


def test_it_reaches_the_sidebar_like_any_other_status():
    """The payoff: a pane reporting over 7501 is readable from another tab,
    through the rows the sidebar already draws for OSC 5577. `working` gets
    the spinner, `done` does not -- which is the distinction 5577's `busy` flag
    was a thin proxy for."""
    side = cfg('tab_bar_side "left"\ntab_bar_width 24\n')
    with Session(shell(), cols=90, rows=20, config=side) as s:
        s.settle(30)
        s.raw(cmd("7501;state=working:msg=" + b64("make test")))
        s.settle(30)
        snap = s.snapshot()
        row = next((y for y in range(20) if "make test" in snap.line(y)[:24]), None)
        check(
            "the status is a row under its tab",
            row is not None,
            repr(snap.line(2)[:24]),
        )
        if row is None:
            return
        check(
            "a `working` record turns the spinner",
            snap.line(row)[1] != " ",
            repr(snap.line(row)[:24]),
        )

        s.raw(cmd("7501;state=done:msg=" + b64("make test")))
        s.settle(30)
        snap = s.snapshot()
        check(
            "`done` is the same words without it",
            "make test" in snap.line(row)[:24] and snap.line(row)[1] == " ",
            repr(snap.line(row)[:24]),
        )


def test_feature_detection_cannot_be_looped():
    """`OSC 7501 ; ?` is answered with the bytes it was sent -- the one shape
    OSC 5577 forbids, because a pane that echoes its input (`cat`, `tee`, a
    shell, an ssh hop to another supporting terminal) turns our answer back
    into a question. 5577 fixed that by naming replies `-reply`; this protocol
    cannot, so the answer is rate-limited instead.

    `emit()` runs `cat -v`, so this pane *is* that case.
    """
    with Session(emit("\\033]7501;?\\033\\\\"), cols=60, rows=12) as s:
        s.settle()
        time.sleep(0.8)  # real time: a loop needs none of our help to run
        out = s.snapshot().pane_text(s.pane())
        n = out.replace("\n", "").replace(" ", "").count("]7501;?")
        check(
            "the query is answered",
            n >= 2,  # the pane's own question, and ours echoed back
            "%d copies: %r" % (n, out[:200]),
        )
        check(
            "and answering it does not start a loop",
            n <= 6,
            "%d copies: %r" % (n, out[:200]),
        )
        check(
            "the pane is still usable afterwards",
            len(out) < 600,
            "%d chars of screen" % len(out),
        )

    # The terminator is the program's choice and the answer has to match it:
    # it is matching on bytes it chose, and a shell script reaches for BEL
    # because a trailing `ESC \` inside a double-quoted string escapes the
    # quote. (libghostty-vt answers the same way upstream.)
    with Session(emit("\\033]7501;?\\x07"), cols=60, rows=12) as s:
        s.settle()
        out = s.snapshot().pane_text(s.pane()).replace("\n", "").replace(" ", "")
        check(
            "a query ended with BEL is answered with BEL",
            "]7501;?^G" in out,
            repr(out[:120]),
        )


def test_the_two_protocols_share_one_slot():
    """`status` and `busy` are what every reader of a pane asks for, so both
    protocols compose into them, last writer wins. Taking the slot takes the
    records too: a `program_state` describing text that is no longer on screen
    would be worse than none."""
    with Session(shell(), cols=70, rows=8) as s:
        s.settle()
        s.raw(cmd("7501;state=working:app=cargo:msg=" + b64("from 7501")))
        s.settle()
        check(
            "7501 has the line", state(s)["program_state"] == "working", str(state(s))
        )

        s.raw(cmd("5577;1;status;from 5577"))
        s.settle()
        st = state(s)
        check("5577 takes the line", st["status"] == "from 5577", str(st))
        check(
            "...and nothing stale is left describing it",
            st["program_state"] == "" and st["program_app"] == "" and not st["busy"],
            str(st),
        )

        s.raw(cmd("7501;state=done:msg=" + b64("back to 7501")))
        s.settle()
        st = state(s)
        check(
            "and 7501 can take it back",
            st["status"] == "back to 7501" and st["program_state"] == "done",
            str(st),
        )

        # a 5577 status before anything speaks 7501 is not cleared by 7501's
        # own clearing rules: those records were never there
        s.raw(cmd("5577;1;status;mine"))
        s.raw(cmd("7501;state=clear"))
        s.settle()
        check(
            "`clear` does not reach into the other protocol's slot",
            state(s)["status"] == "mine",
            str(state(s)),
        )


def test_a_report_split_across_reads():
    with Session(shell(), cols=70, rows=8) as s:
        s.settle()
        s.raw(r'printf "\e]7501;state=work"' + "\\n")
        s.settle(80)
        s.raw(r'printf "ing:msg=aW4gcGllY2Vz"' + "\\n")
        s.settle(80)
        s.raw(r'printf "\x07"' + "\\n")
        s.settle()
        st = state(s)
        check(
            "a report arriving in pieces is still understood",
            st["program_state"] == "working" and st["status"] == "in pieces",
            str(st),
        )


if __name__ == "__main__":
    test_a_report_becomes_the_pane_status()
    test_the_five_states()
    test_a_report_replaces_its_record_whole()
    test_several_records()
    test_clear_takes_the_children_with_it()
    test_records_die_with_the_prompt_and_the_program()
    test_hostile_and_malformed_reports()
    test_a_long_message_is_shortened_not_broken()
    test_it_reaches_the_sidebar_like_any_other_status()
    test_feature_detection_cannot_be_looped()
    test_the_two_protocols_share_one_slot()
    test_a_report_split_across_reads()
    sys.exit(report())
