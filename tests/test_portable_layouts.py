"""A layout file that can be checked in beside the project it describes.

Three things had to be true before a project could own its own layout, and none
of them were: a relative `cwd=` had to mean something (it was handed to chdir
literally, so a project file could only name absolute directories, which are
right on exactly one machine); a dump had to be able to write one tab relative
to a directory (it wrote the whole session, absolute); and a hand-edited layout
had to be checkable, because `cmd=` where `command=` was meant is a shell and no
complaint.
"""

import json
import os
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import BIN, Session, check, report

SH = ["/bin/sh", "-c", "read x"]


def project(name, text):
    """A directory with a layout file in it, the way a checkout would have."""
    root = tempfile.mkdtemp(prefix="sl0ppty-proj-")
    d = os.path.join(root, name)
    os.makedirs(os.path.join(d, "src"))
    path = os.path.join(d, "sl0ppty.layout.kdl")
    with open(path, "w") as f:
        f.write(text)
    return d, path


def lint(path):
    out = subprocess.run([BIN, "--check", str(path)], capture_output=True, text=True)
    return out.returncode, out.stderr + out.stdout


def cwds(s, **kw):
    """Every pane's directory, read off a dump.

    `dump-layout` is the instrument on purpose: it reports where the program in
    the pane *is* (/proc/PID/cwd), so this asserts that the pty really did chdir
    there rather than that a string was stored somewhere."""
    import re

    return re.findall(r'cwd="([^"]*)"', s.api("dump-layout", **kw)["kdl"])


def test_a_relative_cwd_means_the_layout_files_own_directory():
    """The rule `include` already follows for configs, applied to the other half of
    the same syntax: relative to the file that wrote it, never to whatever
    directory the session was started from."""
    d, path = project(
        "api",
        """
layout {
    tab name="api" {
        pane
        pane cwd="src"
    }
}
""",
    )
    with Session(SH, layout=path) as s:
        got = sorted(cwds(s))
        check(
            "a pane with no cwd starts in the layout file's directory",
            got[0] == d,
            str(got) + " want " + d,
        )
        check(
            "and a relative one resolves against it",
            got[1] == os.path.join(d, "src"),
            str(got),
        )


def test_an_absolute_cwd_still_wins():
    """Every layout that already said `/home/you/dev/api` keeps meaning it."""
    d, path = project(
        "api",
        """
layout {
    tab name="api" { pane cwd="/tmp" }
}
""",
    )
    with Session(SH, layout=path) as s:
        check("an absolute cwd is left alone", cwds(s) == ["/tmp"], str(cwds(s)))


def test_a_layout_sent_as_text_has_no_directory_to_be_relative_to():
    """A base is a property of a file. Text arriving over the socket was written by
    whoever sent it, so a relative path there keeps meaning what it always meant
    rather than silently re-rooting somewhere new."""
    with Session(SH) as s:
        started_in = cwds(s)[0]
        s.api(
            "apply-layout", kdl='layout { tab name="t" { pane cwd="." } }', replace=True
        )
        check(
            "a relative cwd in text is not re-rooted against a file",
            cwds(s) == [started_in],
            str(cwds(s)) + " want " + started_in,
        )


def test_a_dump_can_be_asked_for_one_tab_relative_to_a_directory():
    """Which is the whole of what a project's layout file is: this tab, these
    commands, these tags, no absolute paths anywhere in it."""
    d, path = project(
        "api",
        """
layout {
    tab name="api" {
        pane purpose="agent:main"
        pane split="rows" {
            pane cwd="src" command="echo hi" suspended=true
            pane
        }
    }
    tab name="notes" { pane }
}
""",
    )
    with Session(SH, layout=path) as s:
        tabs = s.tabs()
        api = [t for t in tabs if t["name"] == "api"][0]
        whole = s.api("dump-layout")["kdl"]
        check("a whole-session dump has both tabs", whole.count("    tab ") == 2, whole)

        one = s.api("dump-layout", tab=api["id"], relative_to=d)
        kdl = one["kdl"]
        check("asked for one tab, it writes one tab", kdl.count("    tab ") == 1, kdl)
        check("and not the other one", '"notes"' not in kdl, kdl)
        check("the project's own directory is `.`", 'cwd="."' in kdl, kdl)
        check("a directory under it is relative", 'cwd="src"' in kdl, kdl)
        check("and nothing absolute is left", d not in kdl, kdl)
        check("purposes are carried", 'purpose="agent:main"' in kdl, kdl)
        check("it counts the leaves it wrote", one["panes"] == 3, str(one))

        # `active` is a fact about a session, not about a project.
        check("one tab is not marked active", "active=true" not in kdl, kdl)
        check("a whole dump still is", "active=true" in whole, whole)

        bad = s.api("dump-layout", tab=9999)
        check(
            "an unknown tab is refused rather than answered with nothing",
            bad.get("ok") is False,
            str(bad),
        )


def test_a_dump_restores_the_focus_of_every_tab():
    """It used to ask the *current* tab which pane was focused, whichever tab it
    was writing -- so a session of three tabs came back with two of them focused
    wherever first_leaf happened to land."""
    d, path = project(
        "api",
        """
layout {
    tab name="one" { pane; pane focus=true }
    tab name="two" active=true { pane focus=true; pane }
}
""",
    )
    with Session(SH, layout=path) as s:
        kdl = s.api("dump-layout")["kdl"]
        check("every tab says which pane it was in", kdl.count("focus=true") == 2, kdl)


def test_suspend_is_a_policy_because_a_project_is_not_a_session():
    """A dump of a session is honest about what is running. A project's layout must
    not be: the pane running this morning's dev server would start one on every
    open, which is the thing `suspended` exists to prevent."""
    d, path = project(
        "api",
        """
layout {
    tab name="api" {
        pane
        pane command="sleep 60"
    }
}
""",
    )
    with Session(SH, layout=path) as s:
        asis = s.api("dump-layout")
        check(
            "as-is writes what is actually suspended", asis["suspended"] == 0, str(asis)
        )

        cmds = s.api("dump-layout", suspend="commands")
        check(
            "`commands` suspends the pane that was given one",
            cmds["suspended"] == 1,
            str(cmds),
        )
        check(
            "and leaves the shell alone",
            cmds["kdl"].count("suspended=true") == 1,
            cmds["kdl"],
        )

        every = s.api("dump-layout", suspend="all")
        check("`all` means all", every["suspended"] == 2, str(every))
        none = s.api("dump-layout", suspend="none")
        check("`none` means none", none["suspended"] == 0, str(none))
        check(
            "a word nobody knows is refused rather than guessed",
            s.api("dump-layout", suspend="mostly").get("ok") is False,
            str(s.api("dump-layout", suspend="mostly")),
        )


def test_a_layout_that_round_trips_through_a_dump_is_the_same_layout():
    """The property that makes saving a project's layout worth doing at all."""
    d, path = project(
        "api",
        """
layout {
    tab name="api" {
        pane purpose="agent:main"
        pane split="rows" {
            pane cwd="src" command="echo one" suspended=true
            pane purpose="shell:scratch"
        }
    }
}
""",
    )
    with Session(SH, layout=path) as s:
        first = s.api("dump-layout", tab=s.tabs()[0]["id"], relative_to=d)["kdl"]
        again = os.path.join(d, "again.layout.kdl")
        with open(again, "w") as f:
            f.write(first)
    with Session(SH, layout=again) as s2:
        second = s2.api("dump-layout", tab=s2.tabs()[0]["id"], relative_to=d)["kdl"]
    check(
        "the shape, the tags and the directories all survive a round trip",
        first == second,
        first + "\n--- became ---\n" + second,
    )


def test_a_layout_is_checked_as_a_layout():
    """`--check` used to answer a layout with the name of the flag that reads one,
    which is true and no help: the file somebody asked about is the file they want
    checked. One flag, and the document decides which schema it is held to."""
    d, path = project(
        "api",
        """
layout {
    tab name="api" cwd="." {
        pane cmd="pi"
        pane split="diagonal" weight=3 {
            pane command="npm run dev" suspended=yes
            pane purpose="tags-nothing" { pane; pane }
            wobble
        }
    }
}
""",
    )
    code, said = lint(path)
    check("it exits non-zero", code == 1, said)
    for want in [
        "unknown pane property: cmd",
        "split is cols or rows, not `diagonal`",
        "weight is a number of 150 or more",
        "suspended takes true or false, not `yes`",
        "purpose is ignored on a pane with panes in it",
        "a pane holds panes, not `wobble`",
    ]:
        check("it names: " + want, want in said, said)
    check(
        "every problem carries a file and a line",
        said.count("sl0ppty.layout.kdl:") >= 6,
        said,
    )


def test_a_clean_layout_says_so():
    d, path = project(
        "api",
        """
layout {
    tab name="api" { pane purpose="agent:main"; pane command="htop" }
}
""",
    )
    code, said = lint(path)
    check("a layout with nothing wrong exits zero", code == 0, said)
    check("and says which document it read", "a layout" in said, said)


def test_a_layout_with_no_tabs_is_a_layout_that_does_nothing():
    d, path = project("api", "layout {\n}\n")
    code, said = lint(path)
    check("an empty layout is a problem", code == 1, said)
    check("and says which", "declares no tabs" in said, said)


def test_a_config_handed_to_check_is_still_a_config():
    """The discrimination has to keep working in both directions, or telling the
    two documents apart has quietly become one guess."""
    d, _ = project("api", "layout { tab { pane } }\n")
    conf = os.path.join(d, "mine.kdl")
    with open(conf, "w") as f:
        f.write("gap 2\nwobble 3\n")
    code, said = lint(conf)
    check(
        "a config is held to the config schema", "unknown setting: wobble" in said, said
    )
    check("and not the layout one", "unknown pane property" not in said, said)


def test_a_syntax_error_in_a_layout_is_reported_as_the_layout_it_is():
    """A file that will not parse has no top-level names to be recognised by, and
    then the extension is all there is -- which is the job D2 gave it."""
    d, path = project("api", "layout {\n    tab { pane\n")
    code, said = lint(path)
    check(
        "it says which file and which line the syntax broke on",
        "sl0ppty.layout.kdl:" in said and "unclosed" in said,
        said,
    )
    check(
        "and does not talk about config defaults",
        "defaults would stand" not in said,
        said,
    )


def test_the_layout_property_lists_cannot_go_stale():
    """The checker's lists and build_pane's reads are two copies of one schema, and
    the copy that silently gets it wrong is the one that only complains."""
    src = (pathlib.Path(__file__).resolve().parent.parent / "src" / "app.c").read_text()
    start = src.index("/* ---- layouts ---")
    end = src.index("/* ---- resizing and reordering")
    section = src[start:end]

    import re

    read = set(re.findall(r'kdl_prop(?:_int|_bool)?\((?:node|t), "([a-z_]+)"', section))
    listed = set(
        re.findall(
            r'"([a-z_]+)"',
            section[section.index("PANE_PROPS") : section.index("static bool in_list")],
        )
    )
    check("the lists were actually found", len(listed) >= 8, str(listed))
    missing = read - listed
    check(
        "every property the loader reads is in the checker's lists",
        not missing,
        "missing: " + str(sorted(missing)),
    )


def test_a_dump_records_what_a_pane_is_actually_running():
    """`label` only knows what a layout told a pane to run, so a pane you split and
    typed a command into was written back out as a bare shell -- which made setting
    a project up by hand and writing it down two different jobs. The pty has a
    foreground process group, and that group is the job that owns the terminal."""
    with Session(["/bin/sh"]) as s:
        s.raw("sleep 300\\r")
        s.settle(120)
        d = s.api("dump-layout")
        check(
            "the command it is running is in the file",
            'command="sleep 300"' in d["kdl"],
            d["kdl"],
        )
        cmds = s.api("dump-layout", suspend="commands")
        check(
            "and `commands` writes it asleep, so a restore lays it out",
            'command="sleep 300" suspended=true' in cmds["kdl"],
            cmds["kdl"],
        )
        check("counted with the rest", cmds["suspended"] == 1, str(cmds))


def test_a_shell_at_a_prompt_is_not_a_command():
    """Writing `command="bash"` for every idle pane would restore a session where
    every shell runs inside a shell."""
    with Session(["/bin/sh"]) as s:
        s.settle(120)
        d = s.api("dump-layout")
        check(
            "an idle pane is written with no command",
            "command=" not in d["kdl"],
            d["kdl"],
        )
        check(
            "and `commands` finds nothing to suspend",
            s.api("dump-layout", suspend="commands")["suspended"] == 0,
            d["kdl"],
        )


def test_a_background_job_is_not_what_the_pane_is_running():
    """It does not own the terminal. A layout that resurrected `&` jobs in the
    foreground would describe a session nobody had."""
    with Session(["/bin/sh"]) as s:
        s.raw("sleep 300 &\\r")
        s.settle(120)
        check(
            "the pane is still a shell",
            "command=" not in s.api("dump-layout")["kdl"],
            s.api("dump-layout")["kdl"],
        )


def test_an_argument_with_spaces_survives_the_round_trip():
    """`command=` is handed to `/bin/sh -c`, so argv is joined with shell quoting
    or an argument with a space in it comes back as two."""
    with Session(["/bin/sh"]) as s:
        # A long-lived process whose argv holds spaces, a semicolon and quotes.
        s.raw("python3 -c 'import time; time.sleep(300)' \\r")
        s.settle(150)
        kdl = s.api("dump-layout")["kdl"]
        check(
            "the argument is quoted as one word",
            "'import time; time.sleep(300)'" in kdl,
            kdl,
        )

        # Re-running it must rebuild the same argv, which only holds if the
        # quoting is right: python exits non-zero on a broken -c.
        s.api("apply-layout", kdl=kdl, replace=True)
        s.settle(150)
        again = s.api("dump-layout")["kdl"]
        check(
            "and it comes back running the same thing",
            "'import time; time.sleep(300)'" in again,
            again,
        )
        pane = s.panes()[0]
        check(
            "alive, so the command line was not mangled",
            pane["alive"] is True,
            str(pane),
        )


def test_a_layout_declared_command_outranks_what_is_running():
    """It survives the program exiting (D14), and re-saving a project must not
    degrade `npm run dev` into whatever the process tree looks like this minute."""
    d, path = project(
        "api",
        """
layout {
    tab name="api" { pane command="sh -c 'exec sleep 300'" }
}
""",
    )
    with Session(SH, layout=path) as s:
        s.settle(150)
        kdl = s.api("dump-layout")["kdl"]
        check(
            "the file keeps what the layout said",
            """command="sh -c 'exec sleep 300'\"""" in kdl,
            kdl,
        )
        check(
            "not what the kernel would have reported",
            'command="sleep 300"' not in kdl,
            kdl,
        )


def test_a_dead_pane_still_says_what_it_ran():
    """D14: a pane that was given a command outlives it, and so does the record."""
    d, path = project(
        "api",
        """
layout {
    tab name="api" { pane command="echo done-and-gone" }
}
""",
    )
    with Session(SH, layout=path) as s:
        s.settle(200)
        kdl = s.api("dump-layout")["kdl"]
        check(
            "the command is still in the file",
            'command="echo done-and-gone"' in kdl,
            kdl,
        )


if __name__ == "__main__":
    for name, fn in sorted(list(globals().items())):
        if name.startswith("test_"):
            fn()
    sys.exit(report())
