# Layouts

A project's window layout can be defined programatiicaly (and checked in/managed with version control):

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
slosh --layout project.layout
```

## What to call them

**`*.layout`**, and a project's is `slosh.layout`. The syntax is the same KDL
subset the config uses. Point your editor's KDL highlighting at `*.layout`.

## How to verify them

```
$ slosh --check project.layout
project.layout: ok, a layout
```

```
$ slosh --check broken.layout
  broken.layout:2: unknown pane property: cmd
  broken.layout:3: split is cols or rows, not `diagonal`
  broken.layout:4: weight is a number of 150 or more, not `40`
  broken.layout:5: suspended takes true or false, not `yes`
  broken.layout:6: purpose is ignored on a pane with panes in it
  broken.layout:8: a pane holds panes, not `wobble`
broken.layout: 6 problems
```

Those are the mistakes a layout can make that still parses. `this layout
declares no tabs` comes last rather than first, so it reads as the summary it
is.

Loading is the other half of the same answer:

```
$ slosh --layout ~/.config/slosh/config.kdl
slosh: config.kdl: this is a config, not a layout: `gap` is a setting
```

A config handed to `--layout` is named for what it is instead of refused for
having no tabs, which is true of it in the least useful way.

## The layout nodes

- `tab`: one per tab, in strip order. `name=`, `cwd=`, `purpose=`,
  `active=true` for the one you land in.
- `pane`: a leaf, or a split when it has `pane` children. `split="cols"|"rows"`
  says which way it divides; `weight=` is its share of the parent (even shares
  are equal weights, so leaving it out means "even").
- `command=`: what it runs. Without one, a pane runs your shell.
- `cwd=`: where it starts. Inherited from the tab when the pane does not say.
- `suspended=true`: laid out but running nothing until you touch it, so twelve
  checked-out projects are not twelve running dev servers. The pane shows what it
  _would_ run.
- `focus=true`: the pane you start in, within its tab.
- `floating=true`: the pane starts [floating](panes.md#floating-a-pane), with
  `x=` `y=` `w=` `h=` as its wanted rect in cells. All four absent takes the centred default.
- `purpose=`: a label for tooling; see below.

**A relative `cwd=` in a layout _file_ resolves against that file's own
directory**, never against the directory you started the session from. It is
the same rule [`include`](config.md#include) already follows. A layout with no `cwd=` anywhere
starts in the file's directory too, so the common case needs no `cwd=` at all. An
absolute path is unchanged, and `~` is still your home directory.

That is what lets a layout be checked in beside the project it describes and mean
the same thing in everybody's clone:

```kdl
// ~/dev/api/slosh.layout, no absolute path in it anywhere
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

A full annotated example is `config/example.layout`.

## Purposes

A pane or tab can carry a `purpose=` label (`agent:main`, `logs`, `db`) for
tooling to find it by. A purpose declared in a layout or over the control API is
_locked_: a program inside the pane cannot overwrite it, so identity comes from
the layout rather than from whatever the program decides to print.

The [finder](panes.md#finding-a-pane) matches on purposes as well as titles, and
`{"cmd":"panes"}` reports them.

## When the layout is found rather than named

Everything above assumes you name the file. When it lives _in_ the project,
`slosh.layout` beside the `.git`, you do not have to: `C-a w` lists the
projects under the roots you configured and opening one applies the layout it
found there, or a default layout bound to that directory when the project has
none, and `C-a W` writes this tab back out as that file. Same syntax, same
checker, same dump; the only difference is that the path comes from the project
instead of from your command line. That is [workspaces](workspaces.md).
