# Scripting

Three things make slosh easy to drive from other programs: a socket that does
everything the keyboard does, an escape sequence a pane can use to draw its own
chrome, and a headless mode that is the whole program without a terminal.

## The control socket

One JSON object per line, over the session's socket. A detached session answers
exactly as an attached one does.

```bash
$ slosh -s work cmd '{"cmd":"new-tab","name":"api"}'
{"ok":true,"id":2}
$ slosh -s work cmd '{"cmd":"panes"}'
{"ok":true,"panes":[{"id":1,"title":"nvim","alive":true,...}]}
```

Panes and tabs are addressed by **id**, so a background tab is scriptable.

| verb | |
|---|---|
| `panes` `tabs` | what exists, with ids, rects, titles, purposes, state. A pane's `tab_id` is what `move-pane` and `select-tab` want; its `tab` is where that tab sits in the strip |
| `snapshot` | the composited screen, as JSON, `format:"text"`, or `format:"bytes"`, the emitter's own output for this frame (a second call is the delta) |
| `deadline` | when the session wants its next frame, in ms, or -1 |
| `send` | bytes as if typed, decoded like input (`"data":"\\x01\\\\"`) |
| `raw` | bytes straight into the focused pane's pty |
| `resize` | `cols` `rows`, and optionally `cell_w` `cell_h` |
| `split` | `dir:"cols"\|"rows"`, `id` for which pane to split |
| `focus` | `id` |
| `close` `rerun` | `id`, or 0 for the focused pane |
| `clear-shaders` | `id`, or 0 for the focused pane; answers `cleared:0\|1`. The way back from a pane that painted itself unreadable |
| `new-tab` `select-tab` `close-tab` `move-tab` | tabs, by `id` or `index` |
| `move-pane` | `id` of the pane, `tab` id to move it into (`0` for a tab of its own, with an optional `name`), `dir:"cols"\|"rows"`. The pane keeps running: same pty, same scrollback |
| `float` | bare: toggle a pane [floating](panes.md#floating-a-pane) (`id`, or 0 for the focused one). With any of `x` `y` `w` `h` it *places*: floats first when tiled, re-places when floating, never un-floats; omitted fields keep their value |
| `new-float` | a fresh floating shell over the current tab, centred, in the focused pane's directory; answers `id` |
| `set-name` | `target:"tab"` (the default, because that is what this verb has always meant) or `"pane"`, `id`, `name`. A pane accepts 0 for the focused one. A pane's name wins over the title the program sets, so this is how a program that keeps announcing something stale gets overruled; an empty `name` clears it and hands the label back. Refusals say `no such pane` or `no such tab`, so a mistyped target is visible in the reply |
| `set-purpose` | `target:"pane"\|"tab"`, `id` (or 0 for the focused pane, and the tab you are in), `purpose`. An empty `purpose` clears the slot *and* unlocks it, handing the label back to the program |
| `dump-layout` `apply-layout` | see [layouts](layouts.md). `dump-layout` takes `tab` (0 for every tab), `relative_to` to write every `cwd=` under that directory instead of absolute, and `suspend` (`as-is` `none` `commands` `all`); it answers `kdl` `panes` `suspended`, and an unknown `tab` is an error rather than an empty document |
| `workspaces` | the projects on disk and which of them are open: `roots` says whether any are configured at all, and each entry has `name` `path` `purpose` `layout` (a file path, or "") `mtime` `tab` (0 when closed). See [workspaces](workspaces.md) |
| `open-workspace` | `name` or `path`, and `suspended`; answers `tab` `purpose` `path` `created` `tabs` `honoured`. Already open means focused, with `created:false` |
| `close-workspace` | `name` or `purpose`; answers `closed`, how many tabs went |
| `save-workspace` | write this tab as the project's layout: `tab` (0 for the current one), `path` for a tab that is not a workspace yet, `suspend`, and `force` to overwrite a layout the project already has; answers `path` `purpose` `panes` `suspended` `replaced` |
| `notify` | put a line in the session's status area |
| `graphics` | the kitty placements on screen, or the bytes sent for them. The bytes are rendered for *you*, not the client, so anything stateful in them (image transmissions, deletions) is re-sent to the client on its next frame |
| `clipboard` | what the session has copied |
| `reload` | re-read the config; answers `{"ok":true,"warning":...}` if it had a complaint |
| `splash` | replay the attach greeting; `fx` and `motion` pick the colour effect and the assembly by index, for a deterministic one |
| `edit-config` | open the config in a pane |
| `alive` | is it running, and how many panes and tabs |
| `quit` | end the session |

## Driving a project

Everything a program needs in a project it has never seen: open it, ask what is
in it, act on the purposes the project's own layout declared:

```bash
$ slosh -s work cmd '{"cmd":"open-workspace","name":"api"}'
{"ok":true,"tab":3,"purpose":"project:api.5c1f0a3b","path":"/home/you/dev/api","created":true,"tabs":1,"honoured":0}
$ slosh -s work cmd '{"cmd":"open-workspace","name":"api"}'
{"ok":true,"tab":3,"purpose":"project:api.5c1f0a3b","path":"/home/you/dev/api","created":false,"tabs":0,"honoured":0}
```

**The second call focuses what is there and says `created:false`.** Opening is
idempotent, so a script drives it in a loop without asking first. "Have I
opened this already" is the question a script gets wrong after a crash or a
re-attach.

Then read the tab it handed back:

```bash
$ slosh -s work cmd '{"cmd":"panes"}' \
    | jq -c '.panes[] | select(.tab_id == 3) | {id, purpose, suspended}'
{"id":7,"purpose":"agent:main","suspended":false}
{"id":8,"purpose":"service:web","suspended":true}
$ slosh -s work cmd '{"cmd":"rerun","id":8}'    # start the dev server
```

The project's own layout file decided that `service:web` is the dev server and
that it starts asleep; nothing in the session, the config or the calling program
had to know that.

`workspaces` reports each project's layout file `mtime`, so a tool that kept the
mtime it opened a workspace with can tell the file has moved on since, without
the session storing a byte on its behalf. Re-applying the changed layout is
deliberately not offered: the panes it would replace have processes in them.

## A pane can draw its own chrome

By printing an escape sequence. No plugin, no config:

```bash
printf '\033]5577;1;status;building 3/7\033\\'
printf '\033]5577;1;buttons;approve:Approve;cancel:Cancel\033\\'
# clicking [Approve] arrives on the program's stdin as:
#   \033]5577;1;click;approve\033\\
```

The status text appears in the pane's frame; the buttons are real targets in it.
A program that wants to be asked something can ask *in place* rather than
printing a prompt and hoping.

`purpose` is the other verb: `printf '\033]5577;1;purpose;logs\033\\'`. A purpose
declared by a layout or the control API wins and cannot be overwritten this way.

`shader` is the third, and the only one the session can refuse: it sets the shader
passes for the pane that asked, as a document in the config's own syntax, so an
entry's `where=` decides which rect it lands on and the reply counts what went
where. It needs
`in_band_shaders true` because a program restyling your session is a hazard
before it is a convenience. `shader;` with no rect named clears both of that
pane's chains, which is never refused; `clear-shaders` above is the same
thing from outside, for a program that will not do it itself. `shader-load;<path>`
hands over a `shaders { }` file instead of a chain and answers with how much of it
ran (`ok;1 chrome, 0 content`), so a script can apply a preset without knowing how
to read one.
See [shaders](shaders.md#prototyping-in-a-pane).

Anything the session sends *back* to a program ends its verb in `-reply`
(`hello-reply`, `shader-reply`) and no request verb may. A pane that echoes what
it is sent (`cat`, a shell with echo on, a REPL waiting for a line) would
otherwise be answered into a loop, which is exactly what `hello` used to do:
4 MB of hellos in a second and a half.

## Driving it from an agent

Everything above is what an agent needs, and none of it says which parts matter.
`.agents/skills/driving-slosh/SKILL.md` is the same socket written as
instructions: how a program in a pane finds out which session it is in
(`SLOSH_SESSION`, `SLOSH_BIN`), why work belongs in a pane that *was given a
command* rather than typed into somebody's shell, that `alive` and `exit_code` are
how you wait rather than reading the screen for a marker your own echo matches, and
that `purpose` is the handle to find things by because titles change underneath
you.

It follows the `.agents/skills/<name>/SKILL.md` convention, so an agent working in
a checkout picks it up without being told. To use it elsewhere, copy or symlink the
directory into wherever your agent looks for skills. `tests/test_skill.py` checks
every verb, variable and `panes` field it names against the program, because a
stale skill is worse than a missing one: an agent acts on it without a human
reading it first.

## Headless

`slosh --script` is the whole program without a terminal: commands on stdin,
answers on stdout. It is how the test suite works (drive these events, assert
this screen), which is also why the suite is 1,500-odd real end-to-end checks that
finish in about thirteen seconds.

```bash
$ printf '%s\n' '{"cmd":"split","dir":"cols"}' '{"cmd":"snapshot","format":"text"}' \
    | slosh --script --cols 80 --rows 24 -- /bin/sh
```

The bare-verb form (`snapshot text`, `send \x01\\`, `resize 100 30`) is a
human-friendly alias for the same code, so a script and a test cannot drift from
what the API does.
