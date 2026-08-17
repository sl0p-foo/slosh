---
name: driving-sl0ppty
description: >-
  Drive a sl0ppty terminal multiplexer: run a long-lived command somewhere its
  output can be watched, read a pane's screen, arrange panes and tabs, find a
  pane by purpose, open a project workspace, and report progress or ask a
  question from inside a pane. Activates when `SL0PPTY` is set in the
  environment, when a task needs a dev server, log tailer or build left running
  while other work continues, or when the user mentions sl0ppty, panes, tabs or
  workspaces.
---

# Driving sl0ppty

sl0ppty is a terminal multiplexer whose control socket does everything the
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
| `SL0PPTY=1` | this process is running inside a sl0ppty pane |
| `SL0PPTY_SESSION` | the name of the session it is in — empty under `--script` |
| `SL0PPTY_BIN` | the binary that started it |

Use `$SL0PPTY_BIN`, not `sl0ppty`: a session may have been started from a build
tree, and `sl0ppty` is then not on your `PATH`. Both are *set* by the thing that
made your pane, never inherited from whatever started it: `SL0PPTY_SESSION` is
unset under `--script`, which has no socket, rather than carrying the name of a
session the pane is not in. So empty means there is nothing to send commands to,
and it is worth checking before you build a command line out of it:

```bash
[ -n "$SL0PPTY_SESSION" ] || { echo "no session to talk to"; exit 1; }
```

From *outside* a pane, list sessions:

```bash
sl0ppty ls
# main       running
# work       stale
```

`stale` is a socket whose server is gone. Addressing a session that does not
exist prints `sl0ppty: no session named X` and exits 1 — so a failed call is
distinguishable from a call that answered `{"ok":false}`.

## The one interface

```bash
SL=${SL0PPTY_BIN:-sl0ppty}
S="$SL -s $SL0PPTY_SESSION cmd"   # empty means no session, not `main`

$S '{"cmd":"panes"}'
$S '{"cmd":"new-tab","name":"build"}'
```

Every verb has a bare-verb alias for the same code — `$S panes`, `$S tabs`,
`$S alive` — which is convenient at a shell and gives you no arguments. Use JSON
when you need to pass anything.

**Panes and tabs are addressed by `id`, never by index.** A tab whose last pane
closes is removed and every index after it shifts. `id` survives that. Where a
verb takes an `id`, `0` means "the focused pane, and the tab you are looking at".

## Running work so you can read the result

This is the pattern to reach for. Do **not** type a command into somebody's shell
and then screen-scrape for it.

Make a pane that *was given a command*, and tag it:

```bash
$S '{"cmd":"apply-layout","kdl":"layout { tab name=\"build\" { pane purpose=\"task:build\" command=\"make -j8\" } }"}'
```

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
$S '{"cmd":"snapshot","format":"text"}'     # the composited screen as text
$S '{"cmd":"snapshot"}'                     # ...or JSON: rows, styles, cursor
```

`snapshot` is the **whole session's** composited screen — every visible pane, as
laid out, borders and all. To read one pane, take its rect from `panes`
(`content_x`, `content_y`, `content_w`, `content_h`) and cut that window out of
the text.

Two traps:

- **Only what is on screen is in a snapshot.** Scrollback is not. If you need a
  program's whole output, redirect it to a file and read the file; a snapshot is
  for seeing what a human would see.
- **The echoed command line matches your own marker.** Typing `echo DONE` into a
  shell puts the string `DONE` on screen twice: once as the command, once as its
  output. Count occurrences, or use a marker that cannot appear in what you sent.

## Typing into a pane

```bash
$S '{"cmd":"send","data":"ls -la\r"}'   # as if typed: decoded, then re-encoded
$S '{"cmd":"raw","data":"ls -la\r"}'    # straight into the focused pane's pty
```

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
printf '\e]5577;1;buttons;approve:Approve;cancel:Cancel\e\\'
printf '\e]5577;1;purpose;task:build\e\\'
printf '\e]5577;1;clear\e\\'
```

The status shows in the pane's frame. The buttons are real click targets, and a
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
- A session you started for your own work is yours to quit: `sl0ppty -s mine cmd quit`.

## Verbs

| verb | takes |
|---|---|
| `panes` `tabs` | —. ids, rects, titles, purposes, `alive`, `exit_code`, `tab_id` |
| `snapshot` | `format:"text"` for text, omitted for JSON |
| `send` `raw` | `data` |
| `split` | `dir:"cols"\|"rows"`, `id` |
| `focus` `close` `rerun` `clear-shaders` | `id`, or `0` for the focused pane |
| `new-tab` `select-tab` `close-tab` `move-tab` `set-name` | `id` or `index` |
| `move-pane` | `id`, `tab` (`0` for a tab of its own), `dir` |
| `set-purpose` | `target:"pane"\|"tab"`, `id`, `purpose` |
| `apply-layout` | `path` or `kdl`, `replace` |
| `dump-layout` | `tab`, `relative_to`, `suspend` |
| `workspaces` `open-workspace` `close-workspace` `save-workspace` | see above |
| `resize` | `cols` `rows` |
| `notify` | `text` — a line in the session's status area |
| `reload` `edit-config` | — |
| `alive` `deadline` `clipboard` `graphics` | — |
| `quit` | — |

## Testing a script without a session

```bash
printf '%s\n' '{"cmd":"split","dir":"cols"}' '{"cmd":"snapshot","format":"text"}' \
  | sl0ppty --script --cols 80 --rows 24 -- /bin/sh
```

`--script` is the whole program without a terminal: verbs on stdin, answers on
stdout, one line per answering command. `settle <ms>` works here and pumps every
pane until none has produced output for that long, which is how the test suite
avoids sleeping. A multi-line answer (`snapshot text`, `dump-layout`,
`workspaces`) is followed by a blank line; a one-line answer is not.
