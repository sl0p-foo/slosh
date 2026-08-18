# Layouts

A session can be a file, so a project's window layout is checked in with the
project.

```kdl
layout {
    tab name="api" cwd="~/dev/api" {
        pane command="nvim"
        pane split="rows" {
            pane command="npm run dev" suspended=true
            pane
        }
    }
}
```

```bash
slosh --layout project.layout.kdl
```

## What to call them

**`*.layout.kdl`.** The syntax is the same KDL subset the config uses — one
parser reads both files — so the extension says how to read it and `.layout`
says what is in it, the way `docker-compose.yml` and `tsconfig.json` do it.
Calling it something else would trade one wrong signal for another: it *is*
KDL, and an editor that highlights KDL is worth keeping.

A filename cannot enforce anything, so **the document decides which schema it is
held to, not the flag that read it**. `slosh --check` reads what is in front of
it: a file with `layout` or `tab` at the top of it is checked as a layout, and a
config handed to the same flag is still checked as a config. A file too broken to
parse at all is judged by its name, because by then there is nothing else left to
read. One flag rather than a `--check-layout` beside it: naming the schema is the
part you wanted the checker to do, and the answer it used to give — `this is a
layout, not a config` — replied to a question about a file with the name of
another flag.

```
$ slosh --check project.layout.kdl
project.layout.kdl: ok, a layout
```

A clean layout says which schema it passed, because otherwise a layout nothing
could open and a layout with nothing wrong in it read the same. A broken one
reports every problem it found, one indented `file:line: what` per line, then a
summary, and exits 1 — the shape [checking a config](config.md#checking-one)
already has, so one editor compile step and one pre-commit hook read both:

```
$ slosh --check broken.layout.kdl
  broken.layout.kdl:2: unknown pane property: cmd
  broken.layout.kdl:3: split is cols or rows, not `diagonal`
  broken.layout.kdl:4: weight is a number of 150 or more, not `40`
  broken.layout.kdl:5: suspended takes true or false, not `yes`
  broken.layout.kdl:6: purpose is ignored on a pane with panes in it
  broken.layout.kdl:8: a pane holds panes, not `wobble`
broken.layout.kdl: 6 problems
```

Those are the mistakes a layout can make that still parses: a property no pane
has, a `split=` that is neither direction, a `weight=` that is not a number the
engine will honour, a `true`/`false` written some other way, a `purpose=` on a
split — which would tag nothing, because a split runs nothing — and a child node
that is not a `pane`. Last comes `this layout declares no tabs`, reported at the
end rather than first so it reads as the summary it is.

Loading is the other half of the same answer:

```
$ slosh --layout ~/.config/slosh/config.kdl
slosh: config.kdl: this is a config, not a layout: `gap` is a setting
```

A config handed to `--layout` is named for what it is instead of refused for
having no tabs, which is true of it in the least useful way. Both answers come
from the same list of settings the loader reads, so the two cannot disagree about
which file is which.

## The shape

- `tab` — one per tab, in strip order. `name=`, `cwd=`, `purpose=`,
  `active=true` for the one you land in.
- `pane` — a leaf, or a split when it has `pane` children. `split="cols"|"rows"`
  says which way it divides; `weight=` is its share of the parent (even shares
  are equal weights, so leaving it out means "even").
- `command=` — what it runs. Without one, a pane runs your shell.
- `cwd=` — where it starts. Inherited from the tab when the pane does not say.
- `suspended=true` — laid out but running nothing until you touch it, so twelve
  checked-out projects are not twelve running dev servers. The pane shows what it
  *would* run.
- `focus=true` — the pane you start in, within its tab.
- `purpose=` — a label for tooling; see below.

**A relative `cwd=` in a layout *file* resolves against that file's own
directory**, never against the directory you started the session from. It is the
rule [`include`](config.md#include) already follows for configs, applied to the
other half of the same syntax — the two documents share a parser, so they should
not disagree about what a relative path is. A layout with no `cwd=` anywhere
starts in the file's directory too, so the common case needs no `cwd=` at all. An
absolute path is unchanged, and `~` is still your home directory.

That is what lets a layout be checked in beside the project it describes and mean
the same thing in everybody's clone:

```kdl
// ~/dev/api/slosh.layout.kdl — no absolute path in it anywhere
layout {
    tab name="api" cwd="." {
        pane command="nvim"
        pane split="rows" {
            pane cwd="src"
            pane command="npm run dev" suspended=true
        }
    }
}
```

Clone that repo somewhere else and the tab still opens on the checkout, with the
pane that said `src` still in the checkout's `src`. The alternative — absolute
paths, or a `$PROJECT` of our own to expand — makes the file personal to one
machine, or makes you learn a variable to say "here".

A layout that arrives as *text* over the socket has no file, and so no directory
to be relative to. Relative paths in it keep the meaning they had before there
was a base: relative to wherever the session is. Inventing one for text would
mean guessing on behalf of whoever sent it.

A full annotated example is
`config/example.layout.kdl`.

## Writing one back out

```bash
slosh -s work cmd '{"cmd":"dump-layout"}' > project.layout.kdl
```

writes the session as a layout file: tabs, splits, proportions, directories,
commands, which pane you were in. So a session can be checked in, or put back
after a restart.
`contrib/slosh-dev`
is that loop, for when the thing you are rebuilding is slosh itself.

**A pane is dumped with the command it is running**, whether a layout gave it one
or you typed it. `label` — what a layout said — comes first and outlives the
program that ran it, so a project's `command="npm run dev"` stays that in the file
after the server has exited. Failing that, the pane's terminal is asked what owns
it: a pty has a foreground process group, and that group *is* the job running in
the pane. So arranging a project by hand and writing it down are one job rather
than two — split, start the dev server and the log tailer, `C-a W`, done.

Three things are deliberately not a command. **A shell at a prompt**, because
`command="zsh"` on every idle pane would restore a session where each shell runs
inside a shell. **A background job**, because it does not own the terminal and a
layout that resurrected `&` jobs in the foreground would describe a session nobody
had. And **an [ephemeral](panes.md#panes-that-were-given-a-command) pane** — the
editor `C-a e` opened — because restoring one reopens a file somebody finished
with.

Argv is joined with shell quoting, since `command=` is handed to `/bin/sh -c`:
`python3 -c 'import x; x.go()'` comes back as one argument rather than three.

What a dump can honestly restore is the *shape*, and the command that made it.
What it cannot is the state inside a program — a shell's history, a half-written
commit message, an editor's undo — and it does not pretend otherwise. A pane
running the session's default shell is dumped as a pane with no command, so
restoring gives you a fresh one.

Three arguments shape what comes out:

```bash
slosh -s work cmd '{"cmd":"dump-layout","tab":3}'
slosh -s work cmd '{"cmd":"dump-layout","relative_to":"~/dev/api","suspend":"commands"}'
```

| argument | what it says |
|---|---|
| `tab` | a tab **id**, or `0` (the default) for the whole session |
| `relative_to` | a directory every `cwd=` is written relative to |
| `suspend` | which panes come back laid out but not running |

`relative_to` is the writing end of the reading rule above: point it at the
project and the dump comes out with `cwd="."` and `cwd="src"` in it rather than
your home directory, which is what makes the file committable without an edit
afterwards. `suspend` takes four words:

| word | means |
|---|---|
| `as-is` | what is suspended in the session right now — the default here, because a session dump should describe the session |
| `none` | everything comes back running |
| `commands` | every pane that was given a command |
| `all` | nothing starts until you touch it |

`commands` is the one you want for a layout you will open often, and it is what
[workspaces](workspaces.md) saves with: the pane running this morning's dev server
belongs in the file asleep, or opening the project starts a dev server every time.

The reply carries the document as `kdl` and counts beside it — `panes` and
`suspended` — so a caller can report "4 panes, 2 suspended" without parsing back
the document it was handed. A `tab` that does not exist is an error rather than an
empty document, because an empty layout is a plausible answer for a tab you closed
a minute ago and a silent one for a typo in an id.

A dump records `focus=true` in **every** tab, not only the one you were looking
at. It used to ask the session's current tab which pane was focused whichever tab
it was writing, so restoring a six-tab session put you back where you were in one
tab and wherever the file happened to begin in the other five.

## Applying one to a running session

```bash
slosh -s work cmd '{"cmd":"apply-layout","path":"project.layout.kdl"}'
slosh -s work cmd '{"cmd":"apply-layout","kdl":"layout { tab { pane } }","replace":true}'
```

Without `replace`, the tabs the file describes are added to what is already
there.

## Moving a pane between tabs

```bash
slosh cmd '{"cmd":"move-pane","id":3,"tab":2}'              # beside that tab's focus
slosh cmd '{"cmd":"move-pane","id":3,"tab":2,"dir":"rows"}' # under it instead
slosh cmd '{"cmd":"move-pane","id":3,"tab":0,"name":"logs"}' # into a tab of its own
```

The pane keeps running: same pty, same scrollback, same process — a move is tree
surgery, not a new pane and a funeral. The destination is a tab **id** rather than
an index, because a tab whose last pane leaves is removed and every index after it
shifts; an id survives that.

Two things a move deliberately drops, both of them the old tab's opinion rather
than the pane's: a zoom that named it, and its minimised flag. Carried across, the
first would zoom a pane that has left and the second would file the arrival in a
strip nobody asked for. Which tab you are *looking* at does not change.

`C-a >` and `C-a <` push the focused pane one tab along, `C-a b` gives it a tab of
its own, and a pane can be dragged onto a tab in the strip — see
[keys](keys.md#defaults) and [the mouse](panes.md#the-mouse).

## Purposes

A pane or tab can carry a `purpose=` label — `agent:main`, `logs`, `db` — for
tooling to find it by. A purpose declared in a layout or over the control API is
*locked*: a program inside the pane cannot overwrite it, so identity comes from
the layout rather than from whatever the program decides to print.

`C-a P` gives the focused pane one from the keyboard — see
[keys](keys.md#defaults) — and **a purpose an operator typed counts as declared,
so it locks like one from a file**. The lock is not about which door the label
came through, it is about the label coming from outside the pane at all: you named
that pane `service:web` on purpose, and the shell in it printing a title is not an
argument for renaming it.

**Setting a purpose to nothing unlocks it**, from a layout, the API or `C-a P`
with the field cleared, and hands the label back to the program to describe
itself again. Before that, a lock held over an empty string was the one state
nothing could get out of: the pane carried no label, and the lock was what kept
the program from supplying one. It is the same shape as clearing a pane's name,
and worth more than a second verb whose whole job is taking a lock off.

The [finder](panes.md#finding-a-pane) matches on purposes as well as titles, and
`{"cmd":"panes"}` reports them.

## When the layout is found rather than named

Everything above assumes you name the file. When it lives *in* the project —
`slosh.layout.kdl` beside the `.git` — you do not have to: `C-a w` lists the
projects under the roots you configured and opening one applies the layout it
found there, or a default layout bound to that directory when the project has
none, and `C-a W` writes this tab back out as that file. Same syntax, same
checker, same dump; the only difference is that the path comes from the project
instead of from your command line. That is [workspaces](workspaces.md).
