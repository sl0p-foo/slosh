---
name: driving-slosh
description: >-
  House rules and API for driving a slosh terminal multiplexer: spawn
  long-running work as a visible pane IN YOUR OWN TAB (never the human's), read
  a pane's screen, type into a pane, arrange panes and tabs, find a pane by
  purpose, open a project workspace, and report progress or ask a question from
  inside a pane. Use whenever `SLOSH` is set in the environment, and ALWAYS
  before spawning a pane, splitting, applying a layout, moving a pane, or
  reading/typing into one — getting "which pane am I" from focus is the classic
  way to erupt in the middle of somebody else's work.
---

# Driving slosh

slosh is a terminal multiplexer whose control socket does everything the
keyboard does. One JSON object per line in, one JSON object out. A detached
session answers exactly as an attached one does, so nothing here needs a
terminal, a tty, or anybody watching.

Two things make it worth driving rather than shelling out:

- **A pane outlives the command that ran in it.** Output stays readable after the
  program exits, with its exit status, so a build you started five minutes ago is
  still there to read.
- **Panes and tabs carry a `purpose`** — a stable label you choose. It is how you
  find things again. Titles change under you; purposes do not.

## Is there a session, and which one?

Every pane gets these:

| variable | means |
|---|---|
| `SLOSH=1` | this process is running inside a slosh pane |
| `SLOSH_SESSION` | the name of the session it is in — empty under `--script` |
| `SLOSH_BIN` | the binary that started it |
| `SLOSH_PANE` | **which pane this is** — the id every `id:` argument wants |

Use `$SLOSH_BIN`, not `slosh`: a session may have been started from a build
tree, and `slosh` is then not on your `PATH`. Both are *set* by the thing that
made your pane, never inherited from whatever started it: `SLOSH_SESSION` is
unset under `--script`, which has no socket, rather than carrying the name of a
session the pane is not in. So empty means there is nothing to send commands to,
and it is worth checking before you build a command line out of it:

```bash
[ -n "$SLOSH_SESSION" ] || { echo "no session to talk to"; exit 1; }
```

From *outside* a pane, list sessions:

```bash
slosh ls
# main       running
# work       stale
```

`stale` is a socket whose server is gone. Addressing a session that does not
exist prints `slosh: no session named X` and exits 1 — so a failed call is
distinguishable from a call that answered `{"ok":false}`.

## The one interface

```bash
SL=${SLOSH_BIN:-slosh}
S="$SL -s $SLOSH_SESSION cmd"   # empty means no session, not `main`

$S '{"cmd":"panes"}'
$S '{"cmd":"new-tab","name":"build"}'
```

Sixteen verbs also take a bare form — `send raw resize snapshot deadline panes
tabs dump-layout workspaces open-workspace close-workspace save-workspace reload
theme alive quit`. It runs the same code, but **it answers unwrapped**: `panes`
gives you the bare array, `alive` gives you `true`, `snapshot text` gives you the
screen itself. So a filter written for one shape finds nothing in the other:

```bash
$S panes              # [{"id":1,...}]          — no .panes, no .ok
$S '{"cmd":"panes"}'  # {"ok":true,"panes":[...]}
```

That is a silent failure in a `jq` pipeline, and every other verb has no bare
form at all. Type the bare one at a shell; write JSON in anything you keep.

**Panes and tabs are addressed by `id`, never by index.** A tab whose last pane
closes is removed and every index after it shifts. `id` survives that. Where a
verb takes an `id`, `0` means "the focused pane, and the tab you are looking at".

## Rule 1: your own pane is `$SLOSH_PANE`, never the focused one

**`focused` is the human's pane, not yours.** Exactly one pane in the whole
session has `focused:true`, it is always in the tab currently on screen, and it
moves whenever the person watching clicks something. It answers "what is the
human looking at". It does *not* answer "where do I live", and using it for that
is the single most expensive mistake you can make here: work spawned "beside me"
lands in whatever tab somebody else wandered into, and with several agents in one
session it is reliably a stranger's.

```bash
# right: act on the pane you are in
my_tab=$($S '{"cmd":"panes"}' | jq --argjson me "$SLOSH_PANE" '.panes[]|select(.id==$me)|.tab_id')

# wrong: this is wherever the human happens to be
my_tab=$($S '{"cmd":"panes"}' | jq '.panes[]|select(.focused)|.tab_id')
```

If `$SLOSH_PANE` is somehow not set (a scrubbed env, a helper with no
controlling tty, an older slosh), **do not fall back to `focused`** — fall back
to `pid`, which `panes` reports for every pane: walk your own parents
(`/proc/$pid/stat`) until one matches. Both answers are about you; `focused`
never is.

## Running work so you can read the result

This is the pattern to reach for. Do **not** type a command into somebody's shell
and then screen-scrape for it.

Make a pane that *was given a command*, and tag it. `focus:false` builds it
without dragging the view to it — always pass it for background work:

```bash
$S '{"cmd":"apply-layout","focus":false,"kdl":"layout { tab name=\"build\" { pane purpose=\"task:build\" command=\"make -j8\" } }"}'
```

To put it beside *you* rather than in a tab of its own, move it in by id — again
without taking focus:

```bash
id=$($S '{"cmd":"panes"}' | jq '.panes[]|select(.purpose=="task:build")|.id')
$S "{\"cmd\":\"move-pane\",\"id\":$id,\"tab\":$my_tab,\"beside\":$SLOSH_PANE,\"focus\":false}"
```

The pane keeps running across the move: same pty, same scrollback. Panes built
this way are sized properly even in a tab nobody has looked at, so a full-screen
TUI in one renders correctly.

Then poll `panes`, matching on your purpose, until it is no longer alive:

```bash
while :; do
  read -r alive code <<<"$($S '{"cmd":"panes"}' \
    | jq -r '.panes[] | select(.purpose=="task:build") | "\(.alive) \(.exit_code)"')"
  [ "$alive" = "false" ] && break
  sleep 1
done
echo "exit status: $code"
```

`exit_code` is `-1` while it runs, and the real status once it stops
(`exit_signal` is set instead when it was killed). The pane keeps everything it
printed, so read the output whenever you like — see below. A command pane also
offers `{"cmd":"rerun","id":N}`, which runs the same command again in the same
pane, keeping the previous run above it.

**There is no wait primitive over the socket.** `settle` exists only in the
headless driver. `deadline` reports when the *display* wants its next frame and
says nothing about program output — do not use it to wait for work. Poll, with a
sleep you would be happy to explain.

## Reading a pane

```bash
$S "{\"cmd\":\"capture\",\"id\":$id}"       # one pane's visible text
$S '{"cmd":"snapshot","format":"text"}'     # the whole composited screen
$S '{"cmd":"snapshot"}'                     # ...or JSON: rows, styles, cursor
```

**`capture` is the one to use.** It reads that pane's own terminal, so it works
for a pane in a tab that is not on screen, and it changes nothing: no
`select-tab`, no focus change, no effect on a selection the human is holding.
`id:0` is the focused pane.

`snapshot` is the **whole session's** composited screen — every visible pane, as
laid out, borders and all — and only the tab on screen is composited at all. Use
it to see what a *human* sees, not to read a pane. Where a pane sits in that
frame is its rect in `panes` (`content_x`, `content_y`, `content_w`,
`content_h`), which is what to cut out of the text if you are checking the
layout itself rather than a program's output — and the only way to answer "is
this pane on screen, and where", for clicks and geometry.

Two traps:

- **Only what is on screen is in either.** Scrollback is not. If you need a
  program's whole output, redirect it to a file and read the file; these are
  for seeing what a human would see.
- **The echoed command line matches your own marker.** Typing `echo DONE` into a
  shell puts the string `DONE` on screen twice: once as the command, once as its
  output. Count occurrences, or use a marker that cannot appear in what you sent.

## Typing into a pane

```bash
$S '{"cmd":"send","data":"ls -la\r"}'              # as if typed: decoded, then re-encoded
$S "{\"cmd\":\"raw\",\"id\":$id,\"data\":\"ls -la\r\"}"   # straight into that pane's pty
```

**Pass `id` to `raw`.** Without it the bytes go to the focused pane — which is
the human's. Focusing a pane first to type into it is worse: `focus` also
selects that pane's tab, so it moves the view, and it races anybody else doing
the same. `send` has no `id`: it is session input (that is how it can carry the
leader chord), so it always goes where focus is.

`send` goes through the input decoder, so it can carry key chords: `\x01` is the
leader (`C-a` by default), so `{"cmd":"send","data":"\\x01\\\\"}` splits the pane.
`raw` bypasses the decoder, which is what you want for plain text.

The unescaper understands `\e \n \r \t \\ \0 \xHH`. **Write `\e`, never `\033`** —
`\0` is consumed first, so `\033` arrives as a NUL followed by `33`. That is a
silent failure: the bytes go somewhere, nothing happens, and nothing complains.

## Finding things

`purpose` is the handle. Set one when you create a pane, in a layout or over the
socket:

```bash
$S '{"cmd":"set-purpose","id":0,"purpose":"agent:main"}'   # 0 = focused pane
$S '{"cmd":"set-purpose","target":"tab","id":0,"purpose":"notes"}'
```

`panes` and `tabs` report purposes, so finding your own work is a filter and never
a guess:

```bash
$S '{"cmd":"panes"}' | jq '.panes[] | select(.purpose|startswith("task:"))'
```

A purpose set over the socket or by a layout is **declared**, which locks it: a
program running in that pane cannot relabel it. That protects your handle from
whatever a program decides to print — and it means an attempt to overwrite
somebody else's declared purpose is refused rather than silently winning. Do not
take a purpose that is already there; add your own namespace (`task:`, `agent:`,
`svc:`).

Setting a purpose to `""` clears it *and* unlocks it, handing the label back.

## Projects and workspaces

If the user has `project_roots` configured, whole projects are addressable by
name. This is the shortest path from "work on X" to "the panes for X exist":

```bash
$S '{"cmd":"workspaces"}'                            # what exists, and what is open
$S '{"cmd":"open-workspace","name":"api"}'
# {"ok":true,"tab":3,"purpose":"project:api.a1b2c3d4","created":true,...}
```

Opening is **idempotent**: ask twice and the second answers `created:false` and
focuses the tab that is already there. So you can call it without checking first,
which is the call you would otherwise get wrong after a reconnect.

The project's own layout file decided what its panes are and how they are tagged,
so the useful next step is to read them:

```bash
$S '{"cmd":"panes"}' | jq --argjson t 3 '.panes[] | select(.tab_id==$t) | {id,purpose,suspended}'
# {"id":7,"purpose":"agent:main","suspended":false}
# {"id":8,"purpose":"service:web","suspended":true}
$S '{"cmd":"rerun","id":8}'      # start the dev server the project declared
```

A `suspended` pane is laid out and has run nothing — that is how a project keeps
twelve checkouts from being twelve running dev servers. `rerun` starts it.

`{"cmd":"open-workspace","name":"api","suspended":true}` opens everything asleep.
`{"cmd":"save-workspace"}` writes the current tab back out as that project's
layout, recording each pane's directory, command and purpose.

## Reporting from inside a pane

If you are the program *in* a pane, you can draw in your own frame with an escape
sequence. No socket, no config, nothing to set up:

```bash
printf '\e]5577;1;status;building 3/7\e\\'
printf '\e]5577;1;busy;1\e\\'      # ...and it is still going
printf '\e]5577;1;busy;0\e\\'      # ...and now it is not
printf '\e]5577;1;buttons;approve:Approve;cancel:Cancel\e\\'
printf '\e]5577;1;purpose;task:build\e\\'
printf '\e]5577;1;clear\e\\'
```

The status shows in the pane's frame — and, when the session's tab bar is a
sidebar (`tab_bar_side left/right`), as a row under your pane's tab, visible
from every other tab. Treat it as your progress report to somebody working
elsewhere: keep it current while you work ("building 3/7", "tests green"),
and `clear` it when you are done, so a stale line never outlives the work.

**`busy` is the half the words cannot carry.** "building 3/7" reads the same
whether it is building or gave up ten minutes ago, so say which: `busy;1` when
you start something, `busy;0` when it lands. The sidebar draws a busy status in
its own colour with a spinner beside it, which is what somebody in another tab
is actually reading. It is a separate flag, so setting a new status does not
change it; `clear` clears both, and so does your program exiting.
Both are reported by `panes` as `status` and `busy`, so a script can see what
every pane says about itself without reading any screens.
The buttons are real click targets, and a
click arrives **on your stdin** as:

```
\e]5577;1;click;approve\e\\
```

which is how you ask a question in place instead of printing a prompt and hoping
somebody is looking. Button ids are `[A-Za-z0-9_-]`, 1 to 32 characters.

Two rules:

- **A purpose you set this way is in-band, and loses to a declared one.** If the
  pane was tagged by a layout or an operator, your `purpose` is refused. Read
  `panes` if you need to know what you are called.
- **Never treat a reply as a request.** Everything the session sends back ends in
  `-reply` (`hello-reply`, `shader-reply`). If you echo what you are sent — a
  REPL, `cat`, a shell with echo on — do not answer it.

## Leave the session as you found it

- Close what you made: `{"cmd":"close","id":N}` for a pane,
  `{"cmd":"close-tab","id":N}` for a tab, and
  `{"cmd":"close-workspace","name":"api"}` for a workspace. (`close-pane` is the
  *keybinding* action name, not a socket verb.)
- Do not `{"cmd":"quit"}` a session you did not start. It ends every pane in it,
  including the user's.
- A session you started for your own work is yours to quit: `slosh -s mine cmd quit`.

## Verbs

| verb | takes |
|---|---|
| `panes` `tabs` | —. ids, rects, titles, purposes, `alive`, `exit_code`, `pid`, `tab_id`, `purpose_declared`, `floating`, `hidden`, `suspended`, `status`, `busy` |
| `snapshot` | `format:"text"` for text, `"bytes"` for the frame's own output (a second call is the delta), omitted for JSON |
| `capture` | `id` (0 = focused). One pane's visible text, from any tab, with no side effects |
| `send` | `data` — session input, always to the focused pane |
| `raw` | `data`, `id` (0 = focused) — straight into that pane's pty |
| `split` | `dir:"cols"\|"rows"`, `id` |
| `focus` `close` `rerun` `clear-shaders` | `id`, or `0` for the focused pane |
| `new-tab` `select-tab` `close-tab` `move-tab` | `id` or `index` |
| `set-name` | `target:"tab"` (the default) or `"pane"`, `id`, `name`. A pane's name beats the title the program sets, which is how you overrule something that keeps announcing itself; `""` hands the label back |
| `move-pane` | `id`, `tab` (`0` for a tab of its own), `dir`, `beside` (a pane in that tab to land next to), `focus:false` to leave focus and the view alone |
| `float` | `id` to toggle a pane floating; with any of `x` `y` `w` `h` it places instead, and never un-floats |
| `new-float` | —. a floating shell over the current tab, in the focused pane's directory; answers `id` |
| `set-purpose` | `target:"pane"\|"tab"`, `id`, `purpose` |
| `apply-layout` | `path` or `kdl`, `replace`, `focus:false` to build without going there |
| `dump-layout` | `tab`, `relative_to`, `suspend` |
| `workspaces` `open-workspace` `close-workspace` `save-workspace` | see above |
| `resize` | `cols` `rows`, optionally `cell_w` `cell_h` |
| `theme` | —. what is installed and worn; `name` switches now, `save:true` writes it into the config |
| `splash` | `fx` `motion` — replay the attach greeting |
| `notify` | `text` — a line in the session's status area |
| `reload` `edit-config` | — |
| `alive` `deadline` `clipboard` `graphics` | — |
| `quit` | — |

## Testing a script without a session

```bash
printf '%s\n' '{"cmd":"split","dir":"cols"}' '{"cmd":"snapshot","format":"text"}' \
  | slosh --script --cols 80 --rows 24 -- /bin/sh
```

`--script` is the whole program without a terminal: verbs on stdin, answers on
stdout, one line per answering command. `settle <ms>` works here and pumps every
pane until none has produced output for that long, which is how the test suite
avoids sleeping. A multi-line answer (`snapshot text`, `dump-layout`,
`workspaces`) is followed by a blank line; a one-line answer is not.
