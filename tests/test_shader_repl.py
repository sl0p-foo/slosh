#!/usr/bin/env python3
"""`contrib/shader-repl`: the prompt for prototyping chains in a pane.

Two things are worth testing here and one of them is not the prompt. A REPL that
offers tab completion is making a claim about what exists, so the claim is checked
against the code: every shader in `src/shader.c` and every name the expression
language knows in `src/expr.c` has to be in the list, or completion quietly
becomes a list of what was true once.

The rest is the loop itself, driven the way a person drives it -- typed lines,
history, a tab -- in a real session with a real pty.
"""
import ast
import os
import re
import subprocess
import sys
import tempfile
import time

from harness import Session, check, report

# Keys go in escaped (`\\r`, `\\t`, `\\e[A`) rather than as real control characters:
# `raw` takes the rest of one command line, so a literal newline in the argument
# ends the command instead of reaching the pane. Enter is CR, which is what a
# terminal sends and what readline is waiting for.

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPL = os.path.join(ROOT, "contrib", "shader-repl")


def repl_lists():
    """The vocabulary the script offers, read out of the script."""
    src = open(REPL).read()
    def names(var):
        body = re.search(r"^%s = (\[[^\]]*\])" % var, src, re.M).group(1)
        return [w.strip().strip('"\'').rstrip("=(").strip('"')
                for w in re.findall(r'["\'][^"\']+["\']', body)]
    return {v: names(v)
            for v in ("COMMANDS", "SHADERS", "PROPS", "VARS", "CONSTS", "FUNCS")}


def source_names(path, pattern):
    return set(re.findall(pattern, open(os.path.join(ROOT, "src", path)).read()))


def last_reply(sess):
    """The repl's answer to the last thing typed.

    A chain that ran answers with where it went -- `1 chrome, 0 content` -- because
    `where=` inside the text decides that and the count is the only way to see it."""
    txt = sess.snapshot().pane_text(sess.pane())
    for l in reversed([l.strip() for l in txt.split("\n") if l.strip()]):
        if re.match(r"^\d+ chrome, \d+ content$", l) or l == "ok":
            return l
        if l.startswith(("bad ", "no answer", "unknown ", "unexpected ")):
            return l
    return "(no reply)"


def last_line(sess):
    txt = sess.snapshot().pane_text(sess.pane())
    lines = [l.strip() for l in txt.split("\n") if l.strip()]
    return lines[-1] if lines else ""


def cfg(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".kdl", delete=False)
    f.write(text)
    f.close()
    return f.name


def session(cfg_path, data_home, rows=16):
    """A pane running the repl, with its history in a directory we can inspect."""
    env = {"XDG_DATA_HOME": data_home, "TERM": "xterm-256color"}
    return Session(["/bin/sh", "-c", "exec python3 %s" % REPL],
                   cols=84, rows=rows, config=cfg_path, env=env)


def test_the_script_is_a_script():
    """First, because everything below it starts a pane that runs this file: a
    syntax error otherwise shows up as `IndexError: list index out of range` from
    a pane that died before printing its prompt, which is a long way from saying
    line 212 is indented wrong."""
    try:
        ast.parse(open(REPL).read())
        ok, why = True, ""
    except SyntaxError as exc:
        ok, why = False, "%s line %s: %s" % (exc.__class__.__name__, exc.lineno, exc.msg)
    check("contrib/shader-repl parses", ok, why)


def test_the_completion_list_matches_the_code():
    lists = repl_lists()

    # src/shader.c's REGISTRY: `{"dim", sh_dim, ...}`
    shader_c = open(os.path.join(ROOT, "src", "shader.c")).read()
    registry = shader_c[shader_c.index("REGISTRY[] = {"):]
    registry = registry[:registry.index("\n};")]
    builtin = set(re.findall(r'\{"([a-z_-]+)"', registry))
    check("every built-in shader can be completed",
          builtin <= set(lists["SHADERS"]),
          "missing: %s" % sorted(builtin - set(lists["SHADERS"])))
    check("and nothing is offered that is not one",
          set(lists["SHADERS"]) <= builtin,
          "not shaders: %s" % sorted(set(lists["SHADERS"]) - builtin))

    variables = source_names("expr.c", r'\{"([a-z_]+)", V_[A-Z_]+')
    check("every expression variable can be completed",
          variables <= set(lists["VARS"]),
          "missing: %s" % sorted(variables - set(lists["VARS"])))

    consts = source_names("expr.c", r'\{"([A-Z]+)", \d+\},?\s')
    check("every constant can be completed", consts <= set(lists["CONSTS"]),
          "missing: %s" % sorted(consts - set(lists["CONSTS"])))

    funcs = source_names("expr.c", r'\{"([a-z]+)", \d, OP_[A-Z]+\}')
    check("every expression function can be completed",
          funcs <= set(lists["FUNCS"]),
          "missing: %s" % sorted(funcs - set(lists["FUNCS"])))


def test_every_offered_shader_is_accepted_by_a_session():
    """The list is only true if the session agrees, so every name in it gets typed
    at a real prompt. Checked one at a time as each reply lands: a 16-row pane
    cannot hold nine exchanges, and counting `ok`s on a screen that has scrolled
    measures the window, not the run."""
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    names = repl_lists()["SHADERS"]
    refused = []
    with session(path, home) as s:
        s.until_text("chrome>")
        for name in names:
            s.raw("%s amount=40\\r" % name)
            s.settle(60)
            reply = last_reply(s)
            if "content" not in reply:  # `N chrome, M content` means it ran
                refused.append((name, reply))
    check("every completable shader parses in a real session", not refused,
          str(refused))
    os.unlink(path)


def test_history_survives_the_next_run():
    """The point of readline here: what you tried last time is still there. The
    file is written on the way out, so this is two sessions and one directory.

    One press of up reaches the chain even though `:quit` was typed after it,
    because a `:` command is taken back out of the history it would otherwise
    fill."""
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    hist = os.path.join(home, "sl0ppty", "shader-repl.history")

    with session(path, home) as s:
        s.until_text("chrome>")
        s.raw('tint color="#00ff88" amount=200\\r')
        s.until_text("ok")
        s.raw(":quit\\r")
        s.settle(60)
    check("a history file is written", os.path.exists(hist), hist)
    saved = open(hist).read() if os.path.exists(hist) else ""
    check("with the chain that was typed in it", "#00ff88" in saved, repr(saved))
    check("and without the command that ended the run", ":quit" not in saved,
          repr(saved))

    with session(path, home) as s:
        s.until_text("chrome>")
        s.raw("\\e[A")            # up: readline recalls the last line
        s.settle(60)
        line = last_line(s)
        check("and the next run recalls it with one press of up",
              "#00ff88" in line, repr(line))
    os.unlink(path)


def test_tab_completes():
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    with session(path, home) as s:
        s.until_text("chrome>")
        s.raw("grays\\t")         # one match: completed in place
        s.settle(60)
        out = s.snapshot().pane_text(s.pane())
        check("a unique prefix completes to the shader name",
              "grayscale" in out, repr(out[-120:]))

        s.raw(" amount=40\\r")
        s.until_text("ok")
        s.raw(":cl\\t")           # `:c` is ambiguous with :chrome/:content; `:cl` is not
        s.settle(60)
        out = s.snapshot().pane_text(s.pane())
        check("and so does a command", ":clear" in out, repr(out[-120:]))
    os.unlink(path)


def test_load_takes_a_preset_by_name():
    """`:load sine-comet` from any directory: the preset folders sit beside the
    script, so a bare name is enough and the session does the reading."""
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    with session(path, home) as s:
        s.until_text("chrome>")
        pane = s.pane()
        ring = lambda: tuple(
            (s.snapshot().style_at(x, pane["y"]) or {}).get("fg")
            for x in range(pane["x"], pane["x"] + pane["w"]))
        before = {ring() for _ in range(3)}

        s.raw(":load sine-comet\\r")
        s.settle(60)
        out = last_line(s)
        check("it says how much of the file ran",
              "1 chrome, 0 content" in "\n".join(
                  l.strip() for l in
                  s.snapshot().pane_text(pane).split("\n")), repr(out))

        seen = set()
        deadline = time.monotonic() + 3
        while len(seen) < 2 and time.monotonic() < deadline:
            seen.add(ring())
            s.settle(60)
        check("and the preset is running on the frame", len(seen) > 1,
              "%d rings before, %d after" % (len(before), len(seen)))
    os.unlink(path)


def test_load_routes_a_files_entries_by_their_own_where():
    """A preset says which rect it is for, and loading it has to honour that --
    including a file that fills both, which is one exchange and two chains."""
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    both = os.path.join(home, "both.kdl")
    with open(both, "w") as f:
        f.write('// two rects in one file\n'
                'shaders {\n'
                '    tint where="chrome" channel="fg" color="#ff0033" amount=255\n'
                '    tint where="content" channel="bg" color="#00ff88" amount=255\n'
                '}\n')
    with session(path, home) as s:
        s.until_text("chrome>")
        pane = s.pane()
        s.raw(":load %s\\r" % both)
        s.settle(80)
        snap = s.snapshot()
        text = "\n".join(l.strip() for l in snap.pane_text(pane).split("\n"))
        check("both chains are reported", "1 chrome, 1 content" in text, repr(text[-120:]))
        check("the frame took the chrome entry",
              (snap.style_at(pane["x"], pane["y"]) or {}).get("fg") == "#ff0033",
              str(snap.style_at(pane["x"], pane["y"])))
        body = snap.style_at(pane["content_x"] + 1, pane["content_y"] + 1)
        check("and the contents took the other one",
              (body or {}).get("bg") == "#00ff88", str(body))
    os.unlink(path)


def test_load_says_what_is_wrong_rather_than_nothing():
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    plain = os.path.join(home, "notashader.kdl")
    with open(plain, "w") as f:
        f.write('theme { frame_focus "#00ff00" }\n')
    with session(path, home) as s:
        s.until_text("chrome>")
        pane = s.pane()
        cases = [(":load nosuchthing", "no such file"),
                 (":load %s" % plain, "no shaders { } block"),
                 (":load", ":load <file.kdl>")]
        for line, want in cases:
            s.raw(line + "\\r")
            s.settle(60)
            text = "\n".join(l.strip() for l in s.snapshot().pane_text(pane).split("\n"))
            check("`%s` says so: %s" % (line, want), want in text, repr(text[-140:]))
    os.unlink(path)


def test_paste_after_a_load_is_the_file():
    """A loaded preset is a file on disk, so the thing to keep is an `include` of
    it -- not thirty lines reprinted into a pane, and not a rebuild from the chains
    that would drop the comments explaining the effect."""
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    with session(path, home) as s:
        s.until_text("chrome>")
        pane = s.pane()
        s.raw(":load marching-ants\\r")
        s.settle(60)
        s.raw(":paste\\r")
        s.settle(60)
        text = s.snapshot().pane_text(pane)
        check("`:paste` names the file it came from", "marching-ants.kdl" in text,
              repr(text[-200:]))
        # An `include` line rather than the file's thirty: the config has a word
        # for "that file, as written", and it fits on the pane you are looking at.
        check("and offers it as an include", 'include "' in text, repr(text[-200:]))
        check("with a path the session can read",
              "/contrib/chrome/marching-ants.kdl" in text, repr(text[-200:]))
    os.unlink(path)


def test_load_completes_a_preset_name():
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    with session(path, home) as s:
        s.until_text("chrome>")
        s.raw(":load march\\t")
        s.settle(60)
        check("tab after `:load` completes a preset name",
              "marching-ants" in last_line(s), repr(last_line(s)))
    os.unlink(path)


def test_the_prompt_takes_what_a_config_file_says():
    """The grammar is a config's shaders section, not a dialect of it: an entry, a
    `shaders { }` block round it, `include "f.kdl"`, `load f.kdl`. Every one of
    those is a line somebody will paste, because every one of them is a line that
    appears in a config file or in this prompt's own output."""
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    with session(path, home) as s:
        s.until_text("chrome>")
        pane = s.pane()
        for line, want in (
                ('tint color="#ff0033" amount=200', "1 chrome, 0 content"),
                ('shaders { tint color="#ff0033" amount=200 }', "1 chrome, 0 content"),
                ('include "contrib/chrome/heartbeat.kdl"', "heartbeat.kdl"),
                ('load marching-ants', "marching-ants.kdl")):
            s.raw(line + "\\r")
            s.settle(70)
            text = "\n".join(l.strip() for l in
                             s.snapshot().pane_text(pane).split("\n"))
            check("takes `%s`" % line[:38], want in text, repr(text[-160:]))
    os.unlink(path)


def test_a_block_can_be_pasted_over_several_lines():
    """A pasted block arrives a line at a time, so the prompt waits for the brace
    to close before sending anything -- and an empty line gives up on it, because a
    prompt you cannot get out of is worse than one that forgets."""
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    with session(path, home) as s:
        s.until_text("chrome>")
        pane = s.pane()
        for line in ('shaders {',
                     '    tint where="chrome" color="#ff0033" amount=255',
                     '    tint where="content" channel="bg" color="#00ff88" amount=255'):
            s.raw(line + "\\r")
            s.settle(40)
        check("it waits for the closing brace", "...>" in s.snapshot().screen(),
              repr(last_line(s)))
        s.raw("}\\r")
        s.settle(80)
        snap = s.snapshot()
        check("the frame took the chrome entry",
              (snap.style_at(pane["x"], pane["y"]) or {}).get("fg") == "#ff0033",
              str(snap.style_at(pane["x"], pane["y"])))
        body = snap.style_at(pane["content_x"] + 1, pane["content_y"] + 1)
        check("and the contents took the other one",
              (body or {}).get("bg") == "#00ff88", str(body))

        s.raw("shaders {\\r")
        s.settle(40)
        s.raw("\\r")  # an empty line: give up on it
        s.settle(40)
        check("an empty line drops an unfinished block",
              "(dropped)" in s.snapshot().screen(), repr(last_line(s)))
    os.unlink(path)


def test_paste_round_trips_through_the_prompt():
    """The whole point of borrowing the config's syntax: what `:paste` prints is
    something you can type back in and get the same pane. Checked by doing exactly
    that -- read the block off the screen, `:both` to forget it, type it back, and
    compare the cells."""
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    with session(path, home, rows=30) as s:
        s.until_text("chrome>")
        pane = s.pane()
        s.raw('tint color="#ff0033" amount=200\\r')
        s.settle(60)
        s.raw(":content\\r")
        s.settle(40)
        s.raw("dim amount=80\\r")
        s.settle(60)
        before = (
            (s.snapshot().style_at(pane["x"], pane["y"]) or {}).get("fg"),
            (s.snapshot().style_at(pane["content_x"] + 1,
                                   pane["content_y"] + 1) or {}).get("fg"))

        s.raw(":paste\\r")
        s.settle(60)
        lines = [l.strip() for l in s.snapshot().pane_text(pane).split("\n")
                 if l.strip()]
        block = lines[lines.index("shaders {"):]
        block = block[:block.index("}") + 1]
        check("`:paste` printed a block with both rects in it",
              len(block) == 4 and 'where="chrome"' in block[1]
              and 'where="content"' in block[2], str(block))

        s.raw(":both\\r")
        s.settle(60)
        for line in block:
            s.raw(line + "\\r")
            s.settle(40)
        s.settle(80)
        after = (
            (s.snapshot().style_at(pane["x"], pane["y"]) or {}).get("fg"),
            (s.snapshot().style_at(pane["content_x"] + 1,
                                   pane["content_y"] + 1) or {}).get("fg"))
        check("typing it back gives the same pane", before == after,
              "%s -> %s" % (before, after))
    os.unlink(path)


def test_an_unknown_word_says_where_files_go():
    """`include` and `states` are words from the same file and are not shaders. The
    session says `unknown shader`, which is true and unhelpful on its own."""
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    with session(path, home) as s:
        s.until_text("chrome>")
        pane = s.pane()
        s.raw("nonesuch amount=1\\r")
        s.settle(60)
        text = "\n".join(l.strip() for l in s.snapshot().pane_text(pane).split("\n"))
        check("it names the mistake", "unknown shader: nonesuch" in text,
              repr(text[-160:]))
        check("and points at how files are loaded", ":load <path>" in text,
              repr(text[-160:]))
    os.unlink(path)


def test_help_lists_what_there_is():
    path = cfg("in_band_shaders true\n")
    home = tempfile.mkdtemp()
    with session(path, home) as s:
        s.until_text("chrome>")
        s.raw(":help\\r")
        s.settle(60)
        out = s.snapshot().pane_text(s.pane())
        for want in ("spotlight", "curx", "clamp", "ctrl-r"):
            check("`:help` mentions %s" % want, want in out, repr(out[-200:]))
    os.unlink(path)


if __name__ == "__main__":
    test_the_script_is_a_script()
    test_the_completion_list_matches_the_code()
    test_every_offered_shader_is_accepted_by_a_session()
    test_history_survives_the_next_run()
    test_tab_completes()
    test_load_takes_a_preset_by_name()
    test_load_routes_a_files_entries_by_their_own_where()
    test_load_says_what_is_wrong_rather_than_nothing()
    test_paste_after_a_load_is_the_file()
    test_load_completes_a_preset_name()
    test_the_prompt_takes_what_a_config_file_says()
    test_a_block_can_be_pasted_over_several_lines()
    test_paste_round_trips_through_the_prompt()
    test_an_unknown_word_says_where_files_go()
    test_help_lists_what_there_is()
    sys.exit(report())
