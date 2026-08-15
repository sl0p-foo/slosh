#!/usr/bin/env python3
r"""Shaders set by the program running in the pane, over OSC 5577.

D13 said a program must not be able to restyle the session it happens to be
running in. That is still the failure mode, so this is off unless the config says
`in_band_shaders true` -- and with it on, the difference between prototyping a
colour pass and editing a file to guess at one.

The chain is the config's own syntax, and that is the property worth testing:
what you arrive at by typing has to be a line you can paste.
"""
import os
import sys
import tempfile

from harness import Session, check, report

ST = "\\033\\\\"

# A pane's command as a layout file writes it. KDL strings understand \e and have
# no \033, and every quote inside one is escaped -- twice over for the ones that
# have to survive as far as the chain parser.
LAYOUT = r'''layout {
    tab name="t" {
        pane command="/bin/sh -c \"stty raw -echo; printf '\\e]5577;1;shader;chrome;tint color=\\\"#ff0033\\\" amount=255\\x07'; exec cat\""
        pane command="/bin/sh -c \"stty raw -echo; exec cat\""
    }
}
'''


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def osc(payload):
    return "\\033]5577;" + payload + ST


def pane(*seqs, body="hello"):
    """A pane that prints `body`, emits sequences, then echoes what it is sent,
    so the session's replies are readable on screen.

    Each sequence is single-quoted with its `%` doubled: a chain is
    `tint amount="y + 1"`, and inside a double-quoted shell word those quotes
    vanish and the spaces split the argument -- which cost two confusing failures
    before I looked at what the pane was actually running.
    """
    emit = "".join("printf '%s';" % s.replace("%", "%%") for s in seqs)
    return ['/bin/sh', '-c',
            "printf '%s\\n'; stty raw -echo; %s exec cat -v" % (body, emit)]


def staged(*stages, body="hello"):
    """A pane that walks (sequence, marker) stages, waiting for a cue between
    them, so a test can look at the screen *between* two chains.

    A marker proves the session has already processed the sequence in front of
    it, because both arrived on one pty stream in that order -- no sleeping. The
    cue is a bare newline handed over with `raw()`: `send()` turns a newline into
    the CR that Enter really is, and a raw-mode `read` waits for the LF.

    Typing the chains instead was the first attempt and does not work. A reply
    carries no newline, so it sits unread on the pane's stdin and the next line a
    read loop gets is that reply with the typed line glued onto the end of it.
    """
    parts = []
    for i, (seq, mark) in enumerate(stages):
        if i:
            parts.append("read -r _;")
        parts.append("printf '%s'; printf '%s\\n';"
                     % (seq.replace("%", "%%"), mark))
    return ['/bin/sh', '-c',
            "printf '%s\\n'; stty raw -echo; %s exec cat -v"
            % (body, " ".join(parts))]


def frame(snap, p):
    return snap.style_at(p["x"], p["y"])


def replies(snap, p):
    return snap.pane_text(p)


def test_a_program_can_paint_its_own_frame():
    path = cfg("in_band_shaders true\n")
    with Session(pane(osc('1;shader;chrome;tint color="#ff0033" amount=255')),
                 cols=60, rows=9, config=path) as s:
        s.until_text("shader-reply")
        snap, p = s.snapshot(), s.pane()
        st = frame(snap, p)
        check("the frame takes the colour the pane asked for",
              st["fg"] == "#ff0033", str(st))
        check("and the pane is told it was done",
              "shader-reply;ok" in replies(snap, p),
              repr(replies(snap, p)[:120]))
    os.unlink(path)


def test_the_contents_are_a_separate_chain():
    """Two rects, two chains: what runs over the frame is not what runs over the
    text, and setting one leaves the other alone."""
    path = cfg("in_band_shaders true\n")
    with Session(pane(osc('1;shader;content;tint color="#00ff00" amount=255'),
                      osc('1;shader;chrome;tint color="#ff0000" amount=255')),
                 cols=60, rows=9, config=path) as s:
        s.until_text("shader-reply")
        snap, p = s.snapshot(), s.pane()
        body = snap.style_at(p["x"] + 1, p["y"] + 1)
        check("the contents take the content chain",
              body["fg"] == "#00ff00", str(body))
        check("and the frame takes the chrome one",
              frame(snap, p)["fg"] == "#ff0000", str(frame(snap, p)))
    os.unlink(path)


def test_a_line_from_a_config_means_what_it_meant_there():
    """The rect the sender names is the *default*, so an entry lifted out of a
    config file works as written -- and one naming the other rect is refused
    rather than quietly moved to the one that was asked about."""
    path = cfg("in_band_shaders true\n")
    with Session(pane(osc('1;shader;chrome;tint where="chrome" '
                          'color="#0000ff" amount=255')),
                 cols=64, rows=9, config=path) as s:
        s.until_text("shader-reply")
        snap, p = s.snapshot(), s.pane()
        check("a pasted `where=chrome` entry lands on the chrome",
              frame(snap, p)["fg"] == "#0000ff", str(frame(snap, p)))

    with Session(pane(osc('1;shader;chrome;tint where="content" '
                          'color="#0000ff" amount=255')),
                 cols=64, rows=9, config=path) as s:
        s.until_text("shader-reply")
        snap, p = s.snapshot(), s.pane()
        check("and one naming the other rect is refused",
              "error" in replies(snap, p), repr(replies(snap, p)[:160]))
        check("so the frame is left alone",
              frame(snap, p)["fg"] != "#0000ff", str(frame(snap, p)))
    os.unlink(path)


def test_several_entries_in_one_chain():
    """`;` separates entries, which is why the chain is the last field of the
    payload and is taken verbatim rather than split on."""
    path = cfg("in_band_shaders true\n")
    with Session(pane(osc('1;shader;chrome;tint color="#ff0033" amount=255; '
                          'dim amount=120')),
                 cols=60, rows=9, config=path) as s:
        s.until_text("shader-reply")
        snap, p = s.snapshot(), s.pane()
        check("a chain of two entries is accepted whole",
              "shader-reply;ok" in replies(snap, p),
              repr(replies(snap, p)[:120]))
        st = frame(snap, p)
        check("and both ran: tinted, then dimmed off the tint",
              st["fg"] != "#ff0033" and st["fg"] != "#ffffff", str(st))
    os.unlink(path)


def test_an_empty_chain_puts_the_pane_back():
    path = cfg("in_band_shaders true\n")
    with Session(staged((osc('1;shader;chrome;tint color="#ff0033" amount=255'),
                         "on"),
                        (osc("1;shader;chrome;"), "off")),
                 cols=60, rows=9, config=path) as s:
        s.until_text("on")
        painted = frame(s.snapshot(), s.pane())["fg"]
        s.raw("\\n")  # the cue for the next stage
        s.until_text("off")
        after = frame(s.snapshot(), s.pane())["fg"]
        check("the pane was painted", painted == "#ff0033", painted)
        check("and an empty chain gives the frame back", after != painted,
              "%s -> %s" % (painted, after))
    os.unlink(path)


def test_a_chain_that_does_not_read_is_refused_with_a_reason():
    """The rule the config follows: the entry says what it wants, we cannot do
    it, so it does not run -- and the pane is told which part was the problem
    rather than watching nothing happen."""
    path = cfg("in_band_shaders true\n")
    cases = [
        ('1;shader;chrome;bloom amount=200', "unknown shader"),
        ('1;shader;chrome;tint amount="y +"', "bad amount"),
        ('1;shader;chrome;tint channel="blue"', "bad channel"),
        ('1;shader;sideways;tint amount=200', "content or chrome"),
    ]
    for payload, want in cases:
        with Session(pane(osc(payload)), cols=76, rows=9, config=path) as s:
            s.until_text("shader-reply")
            out = replies(s.snapshot(), s.pane())
            check("refused, and says why: %s" % want, want in out,
                  repr(out[:200]))
    os.unlink(path)


def test_a_refused_chain_leaves_the_last_good_one_standing():
    path = cfg("in_band_shaders true\n")
    with Session(staged((osc('1;shader;chrome;tint color="#ff0033" amount=255'),
                         "good"),
                        (osc("1;shader;chrome;nonesuch amount=10"), "bad")),
                 cols=60, rows=9, config=path) as s:
        s.until_text("good")
        before = frame(s.snapshot(), s.pane())["fg"]
        s.raw("\\n")
        s.until_text("bad")
        after = frame(s.snapshot(), s.pane())["fg"]
        check("the good chain ran", before == "#ff0033", before)
        check("and a bad one does not undo it", before == after,
              "%s -> %s" % (before, after))
    os.unlink(path)


def test_it_is_off_unless_the_config_says_otherwise():
    """The D13 default. A program that tries gets told no, which is the part
    that matters: silence would look like a build without the feature."""
    with Session(pane(osc('1;shader;chrome;tint color="#ff0033" amount=255')),
                 cols=70, rows=9) as s:
        s.until_text("shader-reply")
        snap, p = s.snapshot(), s.pane()
        check("the frame is untouched", frame(snap, p)["fg"] != "#ff0033",
              str(frame(snap, p)))
        check("and the pane is told the session will not do it",
              "in_band_shaders is off" in replies(snap, p),
              repr(replies(snap, p)[:200]))


def test_a_pane_can_only_paint_itself():
    """The scope, and the whole of what makes this safe enough to have at all:
    one pane's chain lives on one pane's node, so the loud one cannot reach the
    quiet one beside it.

    A layout rather than a split, because splitting in this harness re-runs the
    session's own command -- two panes both shouting proves nothing about either.
    """
    path = cfg("in_band_shaders true\n")
    lay = cfg(LAYOUT)
    with Session(["/bin/sh", "-c", "exec cat"], cols=90, rows=12,
                 config=path, layout=lay) as s:
        s.until(lambda snap: any(
            snap.style_at(q["x"], q["y"])["fg"] == "#ff0033" for q in s.panes()))
        snap = s.snapshot()
        panes = sorted(s.panes(), key=lambda q: q["x"])
        check("the layout gave two panes", len(panes) == 2, str(len(panes)))
        shouty, quiet = panes[0], panes[1]
        check("the one that asked is painted",
              frame(snap, shouty)["fg"] == "#ff0033", str(frame(snap, shouty)))
        check("and the one beside it is untouched",
              frame(snap, quiet)["fg"] != "#ff0033", str(frame(snap, quiet)))
    os.unlink(path)
    os.unlink(lay)


def test_an_expression_animates():
    """The reason to prototype in-band at all: `t` moves, and a chain that reads
    the clock keeps the session painting without anything else happening."""
    path = cfg("in_band_shaders true\n")
    chain = 'tint color="#ff0033" amount="abs(t / 4 % 510 - 255)"'
    with Session(pane(osc("1;shader;chrome;" + chain)),
                 cols=60, rows=9, config=path) as s:
        s.until_text("shader-reply")
        seen = set()
        for _ in range(14):
            seen.add(frame(s.snapshot(), s.pane())["fg"])
            s.settle(60)
        check("a time expression keeps the frame moving", len(seen) > 2,
              str(sorted(seen)[:6]))
    os.unlink(path)


if __name__ == "__main__":
    test_a_program_can_paint_its_own_frame()
    test_the_contents_are_a_separate_chain()
    test_a_line_from_a_config_means_what_it_meant_there()
    test_several_entries_in_one_chain()
    test_an_empty_chain_puts_the_pane_back()
    test_a_chain_that_does_not_read_is_refused_with_a_reason()
    test_a_refused_chain_leaves_the_last_good_one_standing()
    test_it_is_off_unless_the_config_says_otherwise()
    test_a_pane_can_only_paint_itself()
    test_an_expression_animates()
    sys.exit(report())
