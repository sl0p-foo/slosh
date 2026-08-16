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
sl0ppty --layout project.layout.kdl
```

## What to call them

**`*.layout.kdl`.** The syntax is the same KDL subset the config uses — one
parser reads both files — so the extension says how to read it and `.layout`
says what is in it, the way `docker-compose.yml` and `tsconfig.json` do it.
Calling it something else would trade one wrong signal for another: it *is*
KDL, and an editor that highlights KDL is worth keeping.

A filename cannot enforce anything, so the program says which document it got:

```
$ sl0ppty --check project.layout.kdl
  project.layout.kdl:8: this is a layout, not a config: `sl0ppty --layout` reads those
$ sl0ppty --layout ~/.config/sl0ppty/config.kdl
sl0ppty: config.kdl: this is a config, not a layout: `gap` is a setting
```

Both come from the same list of settings the loader reads, so the two answers
cannot disagree about which file is which.

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

A full annotated example is
`config/example.layout.kdl`.

## Writing one back out

```bash
sl0ppty -s work cmd '{"cmd":"dump-layout"}' > project.layout.kdl
```

writes the session as a layout file: tabs, splits, proportions, directories,
commands, which pane you were in. So a session can be checked in, or put back
after a restart.
`contrib/sl0ppty-dev`
is that loop, for when the thing you are rebuilding is sl0ppty itself.

What a dump can honestly restore is the *shape*. What it cannot is the state
inside a program — a shell's history, a running editor — and it does not pretend
otherwise. A pane running the session's default shell is dumped as a pane with no
command, so restoring gives you a fresh one.

## Applying one to a running session

```bash
sl0ppty -s work cmd '{"cmd":"apply-layout","path":"project.layout.kdl"}'
sl0ppty -s work cmd '{"cmd":"apply-layout","kdl":"layout { tab { pane } }","replace":true}'
```

Without `replace`, the tabs the file describes are added to what is already
there.

## Moving a pane between tabs

```bash
sl0ppty cmd '{"cmd":"move-pane","id":3,"tab":2}'              # beside that tab's focus
sl0ppty cmd '{"cmd":"move-pane","id":3,"tab":2,"dir":"rows"}' # under it instead
sl0ppty cmd '{"cmd":"move-pane","id":3,"tab":0,"name":"logs"}' # into a tab of its own
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

The [finder](panes.md#finding-a-pane) matches on purposes as well as titles, and
`{"cmd":"panes"}` reports them.
