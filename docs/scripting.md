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
| `panes` `tabs` | what exists, with ids, rects, titles, purposes, state. A pane's `tab_id` is what `move-pane` and `select-tab` want; its `tab` is where that tab sits in the strip; its `pid` is the program in it |
| `snapshot` | the composited screen, as JSON, `format:"text"`, or `format:"bytes"`, the emitter's own output for this frame (a second call is the delta) |
| `capture` | one pane's visible text: `id` (0 = focused). Reads the pane's own terminal, so it works for a pane in a tab that is not on screen, and changes nothing — no `select-tab`, no focus, no effect on a selection |
| `deadline` | when the session wants its next frame, in ms, or -1 |
| `send` | bytes as if typed, decoded like input (`"data":"\\x01\\\\"`) |
| `raw` | bytes straight into a pane's pty: `id` (0 = focused). Naming the pane is the point — focusing one first moves the view of whoever is watching |
| `resize` | `cols` `rows`, and optionally `cell_w` `cell_h` |
| `split` | `dir:"cols"\|"rows"`, `id` for which pane to split |
| `focus` | `id` |
| `close` `rerun` | `id`, or 0 for the focused pane |
| `clear-shaders` | `id`, or 0 for the focused pane; answers `cleared:0\|1`. The way back from a pane that painted itself unreadable |
| `new-tab` `select-tab` `close-tab` `move-tab` | tabs, by `id` or `index` |
| `move-pane` | `id` of the pane, `tab` id to move it into (`0` for a tab of its own, with an optional `name`), `dir:"cols"\|"rows"`, `beside` a pane in that tab to land next to (default: whatever it has focused), `focus:false` to leave focus — and the view — where it is. The pane keeps running: same pty, same scrollback |
| `float` | bare: toggle a pane [floating](panes.md#floating-a-pane) (`id`, or 0 for the focused one). With any of `x` `y` `w` `h` it *places*: floats first when tiled, re-places when floating, never un-floats; omitted fields keep their value |
| `new-float` | a fresh floating shell over the current tab, centred, in the focused pane's directory; answers `id` |
| `set-name` | `target:"tab"` (the default, because that is what this verb has always meant) or `"pane"`, `id`, `name`. A pane accepts 0 for the focused one. A pane's name wins over the title the program sets, so this is how a program that keeps announcing something stale gets overruled; an empty `name` clears it and hands the label back. Refusals say `no such pane` or `no such tab`, so a mistyped target is visible in the reply |
| `set-purpose` | `target:"pane"\|"tab"`, `id` (or 0 for the focused pane, and the tab you are in), `purpose`. An empty `purpose` clears the slot *and* unlocks it, handing the label back to the program |
| `dump-layout` `apply-layout` | see [layouts](layouts.md). `apply-layout` takes `focus:false` to build the tabs without going to them (a layout that names an `active` tab still wins); the panes it builds are sized either way. `dump-layout` takes `tab` (0 for every tab), `relative_to` to write every `cwd=` under that directory instead of absolute, and `suspend` (`as-is` `none` `commands` `all`); it answers `kdl` `panes` `suspended`, and an unknown `tab` is an error rather than an empty document |
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
| `theme` | without `name`, what is installed and what is worn (`theme` `themes` `dirs`); with one, switch now, and `save:true` writes the `theme_name` line into the config. See [themes](config.md#themes) |
| `alive` | is it running, and how many panes and tabs |
| `quit` | end the session |

## Your own pane, not the focused one

A program in a pane is told which pane it is in: **`$SLOSH_PANE`**. Everything
else follows from having it.

```bash
$S '{"cmd":"capture","id":'$SLOSH_PANE'}'      # read yourself
$S '{"cmd":"panes"}' | jq --argjson me $SLOSH_PANE '.panes[] | select(.id==$me) | .tab_id'
```

**Do not use `focused` for this.** Exactly one pane in the session is focused,
it is in the tab currently on screen, and it belongs to whoever is looking —
which is not you. A tool that read focus as "where I live" put its panes in
whatever tab the human had wandered into, and in a session with several such
tools running it was reliably somebody else's. `focused` answers "what is the
human doing"; `$SLOSH_PANE` answers "who am I".

So background work is three calls, none of which move anybody's view:

```bash
my_tab=$($S '{"cmd":"panes"}' | jq --argjson me $SLOSH_PANE '.panes[]|select(.id==$me)|.tab_id')

# 1. build a pane that was *given* a command, without going to it
$S '{"cmd":"apply-layout","focus":false,"kdl":"layout { tab name=\"work\" { pane purpose=\"task:build\" command=\"make -j8\" } }"}'
id=$($S '{"cmd":"panes"}' | jq '.panes[]|select(.purpose=="task:build")|.id')

# 2. move it in beside yourself, still without taking focus
$S "{\"cmd\":\"move-pane\",\"id\":$id,\"tab\":$my_tab,\"beside\":$SLOSH_PANE,\"focus\":false}"

# 3. read it wherever it is
$S "{\"cmd\":\"capture\",\"id\":$id}"
```

If you cannot read the environment — a scrubbed env, a helper with no
controlling tty, a process several forks down — `panes` reports each pane's
**`pid`**, so walking your own parents until one of them matches finds your pane
without asking anybody about focus.

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
printf '\033]5577;1;busy;1\033\\'     # ...and it is still going
printf '\033]5577;1;busy;0\033\\'     # ...and now it is not
printf '\033]5577;1;buttons;approve:Approve;cancel:Cancel\033\\'
# clicking [Approve] arrives on the program's stdin as:
#   \033]5577;1;click;approve\033\\
```

The status text appears in the pane's frame; the buttons are real targets in it.
A program that wants to be asked something can ask *in place* rather than
printing a prompt and hoping.

With the tab bar [on a side](panes.md#the-tab-bar), the status is also a row
under the pane's tab — visible from every other tab, and clicking it jumps to
the pane that said it. A status is not decoration on your own frame; it is how
a pane reports progress to somebody working elsewhere, which is a reason to
keep it current and `clear` it when the work is done.

`busy` is the one thing a line of text cannot say about itself: `make test` and
`make test` are the same words whether it is running or finished. It is a flag
of its own rather than a field of `status`, because a status is the *whole*
payload after the verb — `status;a;b;c` is the text `a;b;c` — so anything added
to that line would change what every sender already means. In a sidebar a busy
status gets the spinner (`busy_mark`, coloured `tab_status_spinner`) and
`tab_status_busy`, which is how
"working" and "done" tell themselves apart from another tab without reading the
words. Anything but `0`, `false`, `no` or `off` turns it on; `clear` turns it
off along with the text, and so does the program exiting — a status left behind
by something that is gone describes the past, whatever it last claimed.

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

## ...or say what it is doing in somebody else's protocol

OSC 5577 only works for programs that know slosh exists. **OSC 7501, the
[program status protocol](https://www.superlogical.com/rex/docs/build/program-status),
is the same idea written down for everybody** — by the author of ghostty, and
implemented by it and by Rex — so a program that already speaks it needs to know
nothing about us:

```bash
status() {
  printf '\033]7501;state=%s:msg=%s\033\\' "$1" "$(printf '%s' "$2" | base64 | tr -d '\n')"
}
status working "Syncing photos"
rsync -a ~/Photos backup:/photos && status done "Photos synced" || status error "rsync failed"
```

The body is `key=value` pairs separated by `:`. `state` is the only required
key, and is one of `idle`, `working`, `done`, `blocked`, `error`, or `clear` to
remove the record. The rest: `msg` and `title` (base64 UTF-8), `app` (a stable
name like `cargo`), `progress` (0–100), `kind` (`permission`, `question` or
`auth`, with `blocked`), and `id` to report more than one thing at a time.

It lands in the same place 5577's `status` does — the pane's frame, a row in the
sidebar, `status` and `busy` in `panes` — because every reader of a pane wants
"what is this doing", not "which protocol said so". What the structure buys on
top of that is reported alongside: `program_state`, `program_kind`,
`program_app` and `progress`.

```bash
$ slosh cmd '{"cmd":"panes"}' | jq -c '.panes[] | select(.program_state == "blocked")'
{"id":4,...,"status":"EU West: Approve deploy to production?","busy":true,
 "program_state":"blocked","program_kind":"permission","program_app":"deploy","progress":-1}
```

The two protocols share that one slot, **last writer wins**: a 5577
`status`/`busy`/`clear` drops the 7501 records, because a `program_state`
describing text that is no longer on screen is worse than none. Neither
protocol replaces the other — 7501 is deliberately one-way (a program reports,
the terminal decides what to show), so `buttons`, `click` and `shader` have
nowhere to live in it; and 5577 has no idea what `done` or `blocked` mean.

**A program with several things going at once** gives each an `id`, which may be
a path: `build/test` is a child of `build`. Clearing a record clears everything
under it, a record with no `app` inherits one from its nearest ancestor that has
one, and when several records are true at the same time the pane shows the one
nearest to needing a person: `blocked`, then `error`, `done`, `working`, `idle`,
ties going to whichever was written last. So a deploy that is `working` at the
root while `eu-west` waits for approval shows the approval.

**Records die without anybody saying so**, because there is no heartbeat in this
protocol and a status nobody retracted is the failure mode it exists to fix. A
`working` or `blocked` record is dropped when the program exits or a new shell
prompt begins (`OSC 133 ; A`); `done` and `error` survive both, which is how a
result is still there when you come back to the pane; `idle` stays until it is
replaced; and a full reset (`ESC c`) removes all of them.

To find out whether any of this is listened to, ask: `printf
'\033]7501;?\033\\'` is answered with the same bytes, `\033]7501;?\033\\`, and
by nothing at all in a terminal that does not implement it. That reply *is* a
request — the one shape 5577 forbids — so it is rate-limited to one answer per
250 ms per pane: a program that asks once is answered at once, and a pane that
echoes our answer back into our own scanner stops there instead of trading
megabytes with us.

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
