# Workspaces

A **project** is a directory on disk. A **workspace** is the tab it occupies in a
session. Two words rather than one because the two have different lifetimes: the
project outlives every session and is checked into git, the workspace lasts until
you close the tab.

[Finding a pane](panes.md#finding-a-pane) starts from the complaint this answers --
tabs stop being navigation somewhere around six projects. The finder answers
*where is that pane*; this answers the question before it: which of the things you
work on is this tab about, and what does opening one mean. `C-a w`, a name, enter,
and the tab arrives arranged, because the project itself said how.

## What counts as a project

A subdirectory of a configured root with one of two markers in it:

| marker | what it is |
|---|---|
| `slosh.layout` | a *declared* project -- it says what it needs open |
| `.git` | an *inferred* one -- you work here, but you have not said how |

Anything else is not a project, and nothing is guessed from what is inside it.

**`.git` counts because otherwise the list is empty on the day you configure the
feature.** A picker that only knows declared projects shows nothing until you have
written a layout, and there is nowhere to write one from -- you would need the
picker to get there. The inferred half is also the invitation: such a project reads
`.git · no layout` in the picker, and `C-a W` is how it stops being one.

**A directory that is a project is never descended into.** That is what keeps the
scan out of `node_modules`, a vendored checkout or a submodule without a rule about
any of their names -- they are under a project, so the walk stops above them. A skip
list is a list of the directories that were fashionable when it was written; this
holds for the ones nobody has invented yet.

## Where they live

Two settings, both in your [config](config.md):

```kdl
project_roots "~/dev" "~/work" depth=2
project_layout "~/.config/slosh/project.layout"
```

`project_roots` takes more than one directory because people have more than one --
work in one tree, everything else in another -- and `depth` is how many levels below
each root to look, 2 by default, 1 to 8. Two is `~/dev/thing` and
`~/dev/org/thing`, which is where checkouts actually sit.

**No roots configured and the feature is dormant.** Nothing is scanned, `C-a w`
says which setting is missing rather than opening an empty picker, and `workspaces`
answers `roots: false` -- because "you have no projects" and "you never said where
they are" are different facts, and a tool that cannot tell them apart reports the
wrong one.

`project_layout` is what a project with no file of its own opens as. **Relative
paths in it bind to the project being opened, not to the directory the file lives
in** -- deliberately the opposite of the rule for a layout you name yourself, where
a relative `cwd=` is relative to that file ([layouts](layouts.md#the-shape)). The
whole point of one shared layout is that `cwd="."` means *this* project; bound to
itself it would open every project in `~/.config/slosh`. Unset, a project opens
as one pane running your shell in it.

**Nothing sniffs `package.json` or `Cargo.toml` to guess what to run.** A table
mapping manifests to commands is wrong for somebody the day it ships -- the repo
with two package managers, the one whose dev server is behind a make target -- and
stale for everybody a year later. `project_layout` is that table with your name on
it, covering the projects you have rather than the ones a heuristic imagined, and a
project that disagrees with it carries its own file.

## From the keyboard

| key | does |
|---|---|
| `w` | the projects picker |
| `W` | write this tab as this project's layout |
| `P` | set this pane's purpose |

`w` and `W` are the same shifted pair as `p`/`P`: go to one, write one down. All
three are bindable as `workspaces`, `save-workspace` and `set-purpose`, and all
three are in the palette under `projects`, so they work before you have rebound
anything -- see [keys](keys.md#defaults) and [every action](keys.md#every-action).

The picker narrows on a project's name, its path and which marker it has, so typing
`.git` gives you the projects with no layout yet -- which is the list you want when
you are about to write one. A project that is already open shows its purpose
instead, and a dot marks the workspace you are in.

The flow the feature is shaped around, once through:

```
C-a w                       picker: ~/work  newthing   .git · no layout
Enter                       your project_layout, cwd bound to newthing
C-a Enter, npm run dev      arrange it, run things
C-a P  service:web  Enter   tag the pane that matters
C-a W                       "wrote slosh.layout · 4 panes, 2 suspended"
```

Tomorrow, `C-a w Enter` rebuilds that tab with the dev server in the pane you put
it in and not running. A tab with no name of its own takes the project's, because a
strip reading `1 2 3` is the thing workspaces are for.

**Closing one is `C-a X`**, which closes the tab and the workspace with it --
membership *is* the tab's purpose, so there is nothing else to put away and no
second key to learn ([keys](keys.md#defaults)). `close-workspace` over the socket
is the same thing for a workspace whose layout opened several tabs: it closes
every tab carrying that purpose and says how many.

## Saving one

`C-a W` writes the focused tab to `slosh.layout` in the project's directory,
and the toast says what happened:
`wrote slosh.layout · 4 panes, 2 suspended`.

**What each pane is running goes in with it.** That is what makes writing a layout
the same act as arranging one: split the tab, start the dev server, start the log
tailer, tag the ones that matter, `C-a W`. A pane's terminal has a foreground
process group and that group is the job running in it, so nothing has to be
declared up front for it to be recorded -- see
[writing one back out](layouts.md#writing-one-back-out) for what is deliberately
*not* counted as a command (a shell at a prompt, a background job, an ephemeral
pane).

**A project's layout is not a verbatim dump of the session**, and `suspend` is
where the two part company. It takes one of four values:

| `suspend` | writes `suspended=true` on |
|---|---|
| `as-is` | whatever is suspended right now -- the default for `dump-layout` |
| `none` | nothing |
| `commands` | every pane that has a command -- the default for `save-workspace` |
| `all` | every pane |

The defaults differ because the two verbs answer different questions.
[`dump-layout`](layouts.md#writing-one-back-out) is *put this session back*, so it
reports the state it found. `save-workspace` is *this is what this project needs
open*, and the pane running this morning's dev server is a pane that should be laid
out tomorrow rather than started -- twelve checked-out projects are not twelve
running dev servers ([layouts](layouts.md#the-shape)). A suspended pane still shows
what it would run, so the layout stays readable as a record of the commands.

Neither can restore what is *inside* a program, and the file does not pretend
otherwise: a shell's history and an editor's buffers are not in it.

**Over the API, writing over a layout that is already there is refused without
`force`; from the keyboard `C-a W` replaces it and the toast says `replaced`.**
That asymmetry is not an oversight in either direction. A key cannot pass a flag,
and the operator pressing it is looking at the tab being written; the file is
committed text, so the diff is the confirmation and the replacement stays
reviewable after the fact. A script has none of that -- no witness, no glance at
the screen -- so it says so in the request.

`save-workspace` also takes `path`, for a tab that is not a workspace yet: that is
how a hand-arranged tab becomes a project's layout without opening it through the
picker first.

`slosh --check slosh.layout` holds the result to the layout schema and
prints one `file:line: what` per problem, so a project's layout goes through the
same check as everything else you commit -- see
[layouts](layouts.md#what-to-call-them).

## Identity

A workspace *is* its tab's [purpose](layouts.md#purposes):

```
project:api.9f3c1d20
```

The hash is of the resolved absolute path, not of the name, so two git worktrees of
one repo are two workspaces -- which is what they are, and a name-keyed session
would have merged them and then focused the wrong one. Membership is that string
and nothing else: no side table of open projects to fall out of step with the tabs.

Because the session declared it, it is **locked** -- no program running in a pane
can relabel a project tab, however chatty its title. That is the same rule any
declared purpose gets, and it is what makes the purpose safe for a tool to address.

A tab that a layout gave some *other* purpose keeps it and is not a member.
Honouring it is the point: overwriting a declared purpose is precisely what the
lock forbids, and a project layout that names a tab `agent:main` meant it.
`open-workspace` counts those in `honoured`, so a caller can see that a tab was
built and deliberately not claimed.

## Nothing watches

Discovery is derived, never remembered: one `readdir` per root plus two `stat`s per
entry, run when the picker opens rather than per frame. **Nothing is stored, so
nothing can be stale** -- and no watcher can promise that. Inotify does not fire for
a bind mount, an sshfs share or a checkout another machine wrote, so a cache that
trusted it would be wrong exactly when the answer mattered.

One thing a scan cannot see is a project's layout file changing under a workspace
that is already open. `workspaces` reports each layout's `mtime`, so a tool that
keeps the mtime it opened with can compare and notice. Re-applying the new file over
the running panes is deliberately not offered -- it would kill them.

## From another program

Open one and act on what is in it, over the [control
socket](scripting.md#the-control-socket):

```bash
$ slosh -s work cmd '{"cmd":"open-workspace","name":"api"}'
{"ok":true,"tab":3,"purpose":"project:api.9f3c1d20","created":true,...}
$ slosh -s work cmd '{"cmd":"panes"}'      # keep the ones whose tab_id is 3
```

| verb | takes | answers |
|---|---|---|
| `workspaces` | -- | `roots`, and a `workspaces[]` of `name` `path` `purpose` `layout` (the file, or `""`) `mtime` `tab` (0 when it is not open) |
| `open-workspace` | `name` or `path`; `suspended` | `tab` `purpose` `path` `created` `tabs` `honoured`. Already open, it focuses that tab and answers `created:false` |
| `close-workspace` | `name` or `purpose` | `closed` -- how many tabs went |
| `save-workspace` | `tab` (0 for the current one), `path` for a tab that is not a workspace yet, `suspend`, `force` | `path` `purpose` `panes` `suspended` `replaced` |

Every one of them is in the bare dispatcher too, so `slosh -s work cmd
workspaces` and `slosh -s work cmd open-workspace api` do the same from a shell
without quoting JSON ([scripting](scripting.md#the-control-socket)).

`open-workspace` is idempotent, which is the property that makes it safe to call
from a script that does not know whether it ran already: asking twice focuses the
tab rather than building a second one. `suspended:true` opens the layout with every
pane suspended whatever the file said -- the *open ten projects, run zero processes*
case, which is a different question from the one the project answered about which
of its own panes are expensive.

The useful part is the last step. Having opened a workspace, filter
[`panes`](scripting.md#the-control-socket) by that `tab_id` and act on the purposes
the project's own layout declared: the project decided that `service:web` is its
dev server and that it starts suspended, and neither the session nor the program
driving it had to know that beforehand.
