# Workspaces

slosh has the concept of **workspaces**. Essentially a workspace is a 
pane layout that represents a project. slosh will scan your config's `project_roots`
folder(s) for two kinds of sub folders: ones that have a `slosh.layout` file, and
ones that have a `.git` but no `slosh.layout`. in the latter case our project picker
can still pick it up and help you provision a new `slosh.layout`.

## Configuration options

Two settings, both in your [config](config.md):

```kdl
project_roots "~/dev" "~/work" depth=2
project_layout "~/.config/slosh/project.layout"
```

`project_roots` takes more than one directory because people have more than one
(work in one tree, everything else in another), and `depth` is how many levels below
each root to look, 2 by default, 1 to 8.

## From the keyboard

| key | does |
|---|---|
| `w` | the projects picker |
| `W` | write this tab as this project's layout |
| `P` | set this pane's purpose |

`w` and `W` are the same shifted pair as `p`/`P`: go to one, write one down. All
three are bindable as `workspaces`, `save-workspace` and `set-purpose`, and all
three are in the palette under `projects`, so they work before you have rebound
anything; see [keys](keys.md#defaults) and [every action](keys.md#every-action).

The picker narrows on a project's name, its path and which marker it has, so typing
`.git` gives you the projects with no layout yet, which is the list you want when
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

**Closing one is `C-a X`**, which closes the tab and the workspace with it:
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
declared up front for it to be recorded; see
[writing one back out](layouts.md#writing-one-back-out) for what is deliberately
*not* counted as a command (a shell at a prompt, a background job, an ephemeral
pane).

**A project's layout is not a verbatim dump of the session**, and `suspend` is
where the two part company. It takes one of four values:

| `suspend` | writes `suspended=true` on |
|---|---|
| `as-is` | whatever is suspended right now, the default for `dump-layout` |
| `none` | nothing |
| `commands` | every pane that has a command, the default for `save-workspace` |
| `all` | every pane |

The defaults differ because the two verbs answer different questions.
[`dump-layout`](layouts.md#writing-one-back-out) is *put this session back*, so it
reports the state it found. `save-workspace` is *this is what this project needs
open*, and the pane running this morning's dev server is a pane that should be laid
out tomorrow rather than started: twelve checked-out projects are not twelve
running dev servers ([layouts](layouts.md#the-shape)). A suspended pane still shows
what it would run, so the layout stays readable as a record of the commands.

Neither can restore what is *inside* a program, and the file does not pretend
otherwise: a shell's history and an editor's buffers are not in it.

**Over the API, writing over a layout that is already there is refused without
`force`; from the keyboard `C-a W` replaces it and the toast says `replaced`.**
The asymmetry is deliberate: the operator pressing the key is looking at the
tab being written, and the file is committed text, so the diff is the
confirmation. A script has no witness, so it says so in the request.

`save-workspace` also takes `path`, for a tab that is not a workspace yet: that is
how a hand-arranged tab becomes a project's layout without opening it through the
picker first.

`slosh --check slosh.layout` holds the result to the layout schema and
prints one `file:line: what` per problem, so a project's layout goes through the
same check as everything else you commit; see
[layouts](layouts.md#what-to-call-them).

## Identity

A workspace *is* its tab's [purpose](layouts.md#purposes):

```
project:api.9f3c1d20
```

The hash is of the resolved absolute path, not of the name, so two git worktrees of
one repo are two workspaces, which is what they are; a name-keyed session
would have merged them and then focused the wrong one. Membership is that string
and nothing else: no side table of open projects to fall out of step with the tabs.

Because the session declared it, it is **locked**: no program running in a pane
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
nothing can be stale**, and no watcher can promise that. Inotify does not fire for
a bind mount, an sshfs share or a checkout another machine wrote, so a cache that
trusted it would be wrong exactly when the answer mattered.

One thing a scan cannot see is a project's layout file changing under a workspace
that is already open. `workspaces` reports each layout's `mtime`, so a tool that
keeps the mtime it opened with can compare and notice. Re-applying the new file over
the running panes is deliberately not offered: it would kill them.

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
| `close-workspace` | `name` or `purpose` | `closed`, how many tabs went |
| `save-workspace` | `tab` (0 for the current one), `path` for a tab that is not a workspace yet, `suspend`, `force` | `path` `purpose` `panes` `suspended` `replaced` |

Every one of them is in the bare dispatcher too, so `slosh -s work cmd
workspaces` and `slosh -s work cmd open-workspace api` do the same from a shell
without quoting JSON ([scripting](scripting.md#the-control-socket)).

`open-workspace` is idempotent: asking twice focuses the tab rather than
building a second one, so a script that does not know whether it already ran is
safe. `suspended:true` opens the layout with every
pane suspended whatever the file said: the *open ten projects, run zero processes*
case, which is a different question from the one the project answered about which
of its own panes are expensive.

The useful part is the last step. Having opened a workspace, filter
[`panes`](scripting.md#the-control-socket) by that `tab_id` and act on the purposes
the project's own layout declared: the project decided that `service:web` is its
dev server and that it starts suspended, and neither the session nor the program
driving it had to know that beforehand.
