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

`project_roots` takes one or more director{y,ies}, and `depth` is how many levels below
each root to look, 2 by default.

## From the keyboard

| key | action         | does                                    |
| --- | -------------- | --------------------------------------- |
| `w` | workspaces     | the projects picker                     |
| `W` | save-workspace | write this tab as this project's layout |
| `P` | set-purpose    | set this pane's purpose                 |

The picker narrows on a project's name, its path and which marker it has, so typing
`.git` gives you the projects with no layout yet, which is the list you want when
you are about to write one. A project that is already open shows its purpose
instead, and a dot marks the workspace you are in.

**Closing one is `C-a X`**, which closes the tab and the workspace with it.

## Saving one

`C-a W` writes the focused tab to `slosh.layout` in the project's directory.

**What each pane is running goes in with it.** That is what makes writing a layout
the same act as arranging one: split the tab, start the dev server, start the log
tailer, tag the ones that matter, `C-a W`. A pane's terminal has a foreground
process group and that group is the job running in it, so nothing has to be
declared up front for it to be recorded.

`save-workspace` also takes `path`, for a tab that is not a workspace yet: that is
how a hand-arranged tab becomes a project's layout without opening it through the
picker first.

`slosh --check slosh.layout` holds the result to the layout schema and
prints one `file:line: what` per problem, so a project's layout goes through the
same check as everything else.

## Identity

A workspace _is_ its tab's [purpose](layouts.md#purposes):

```
project:api.9f3c1d20
```

The hash is of the resolved absolute path, not of the name, so two git worktrees of
one repo are two workspaces, which is what they are; a name-keyed session
would have merged them and then focused the wrong one. Membership is that string
and nothing else: no side table of open projects to fall out of step with the tabs.

Because the session declared it, it is **locked**: no program running in a pane
can relabel a project tab, however chatty its title.

## From another program

Open one and act on what is in it, over the [control
socket](scripting.md#the-control-socket):

```bash
$ slosh -s work cmd '{"cmd":"open-workspace","name":"api"}'
{"ok":true,"tab":3,"purpose":"project:api.9f3c1d20","created":true,...}
$ slosh -s work cmd '{"cmd":"panes"}'      # keep the ones whose tab_id is 3
```

| verb              | takes                                                                                           | answers                                                                                                                     |
| ----------------- | ----------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `workspaces`      | --                                                                                              | `roots`, and a `workspaces[]` of `name` `path` `purpose` `layout` (the file, or `""`) `mtime` `tab` (0 when it is not open) |
| `open-workspace`  | `name` or `path`; `suspended`                                                                   | `tab` `purpose` `path` `created` `tabs` `honoured`. Already open, it focuses that tab and answers `created:false`           |
| `close-workspace` | `name` or `purpose`                                                                             | `closed`, how many tabs went                                                                                                |
| `save-workspace`  | `tab` (0 for the current one), `path` for a tab that is not a workspace yet, `suspend`, `force` | `path` `purpose` `panes` `suspended` `replaced`                                                                             |

Every one of them is in the bare dispatcher too, so `slosh -s work cmd
workspaces` and `slosh -s work cmd open-workspace api` do the same from a shell
without quoting JSON ([scripting](scripting.md#the-control-socket)).

`open-workspace` is idempotent: asking twice focuses the tab rather than
building a second one, so a script that does not know whether it already ran is
safe. `suspended:true` opens the layout with every
pane suspended whatever the file said: the _open ten projects, run zero processes_
case, which is a different question from the one the project answered about which
of its own panes are expensive.
