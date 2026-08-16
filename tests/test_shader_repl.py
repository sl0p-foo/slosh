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
    """The repl's answer to the last chain: `ok`, or why not."""
    txt = sess.snapshot().pane_text(sess.pane())
    for l in reversed([l.strip() for l in txt.split("\n") if l.strip()]):
        if l == "ok" or l.startswith("bad ") or l.startswith("no answer"):
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


def session(cfg_path, data_home):
    """A pane running the repl, with its history in a directory we can inspect."""
    env = {"XDG_DATA_HOME": data_home, "TERM": "xterm-256color"}
    return Session(["/bin/sh", "-c", "exec python3 %s" % REPL],
                   cols=84, rows=16, config=cfg_path, env=env)


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
            if reply != "ok":
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
    test_help_lists_what_there_is()
    sys.exit(report())
