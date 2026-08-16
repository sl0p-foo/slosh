# Scripting

Three things make sl0ppty easy to drive from other programs: a socket that does
everything the keyboard does, an escape sequence a pane can use to draw its own
chrome, and a headless mode that is the whole program without a terminal.

## The control socket

One JSON object per line, over the session's socket. A detached session answers
exactly as an attached one does.

```bash
$ sl0ppty -s work cmd '{"cmd":"new-tab","name":"api"}'
{"ok":true,"id":2}
$ sl0ppty -s work cmd '{"cmd":"panes"}'
{"ok":true,"panes":[{"id":1,"title":"nvim","alive":true,...}]}
```

Panes and tabs are addressed by **id**, so a background tab is scriptable.

| verb | |
|---|---|
| `panes` `tabs` | what exists, with ids, rects, titles, purposes, state |
| `snapshot` | the composited screen, as JSON or `format:"text"` |
| `deadline` | when the session wants its next frame, in ms, or -1 |
| `send` | bytes as if typed, decoded like input (`"data":"\\x01\\\\"`) |
| `raw` | bytes straight into the focused pane's pty |
| `resize` | `cols` `rows`, and optionally `cell_w` `cell_h` |
| `split` | `dir:"cols"\|"rows"`, `id` for which pane to split |
| `focus` | `id` |
| `close` `rerun` | `id`, or 0 for the focused pane |
| `clear-shaders` | `id`, or 0 for the focused pane; answers `cleared:0\|1` — the way back from a pane that painted itself unreadable |
| `new-tab` `select-tab` `close-tab` `move-tab` `set-name` | tabs, by `id` or `index` |
| `move-pane` | `id` of the pane, `tab` id to move it into (`0` for a tab of its own, with an optional `name`), `dir:"cols"\|"rows"`. The pane keeps running — same pty, same scrollback |
| `set-purpose` | `target:"pane"\|"tab"`, `id`, `purpose` |
| `dump-layout` `apply-layout` | see [layouts](layouts.md) |
| `notify` | put a line in the session's status area |
| `graphics` | the kitty placements on screen, or the bytes sent for them |
| `clipboard` | what the session has copied |
| `reload` | re-read the config; answers `{"ok":true,"warning":...}` if it had a complaint |
| `edit-config` | open the config in a pane |
| `alive` | is it running, and how many panes and tabs |
| `quit` | end the session |

## A pane can draw its own chrome

By printing an escape sequence — no plugin, no config:

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
passes for the pane that asked -- a document in the config's own syntax, so an
entry's `where=` decides which rect it lands on and the reply counts what went
where. It needs
`in_band_shaders true` because a program restyling your session is a hazard
before it is a convenience. `shader;` with no rect named clears both of that
pane's chains, which is never refused -- and `clear-shaders` above is the same
thing from outside, for a program that will not do it itself. `shader-load;<path>`
hands over a `shaders { }` file instead of a chain and answers with how much of it
ran (`ok;1 chrome, 0 content`), so a script can apply a preset without knowing how
to read one.
See [shaders](shaders.md#prototyping-in-a-pane).

Anything the session sends *back* to a program ends its verb in `-reply` --
`hello-reply`, `shader-reply` -- and no request verb may. A pane that echoes what
it is sent (`cat`, a shell with echo on, a REPL waiting for a line) would
otherwise be answered into a loop, which is exactly what `hello` used to do:
4 MB of hellos in a second and a half.

## Headless

`sl0ppty --script` is the whole program without a terminal: commands on stdin,
answers on stdout. It is how the test suite works — drive these events, assert
this screen — which is also why the suite is 700-odd real end-to-end checks that
finish in eight seconds.

```bash
$ printf '%s\n' '{"cmd":"split","dir":"cols"}' '{"cmd":"snapshot","format":"text"}' \
    | sl0ppty --script --cols 80 --rows 24 -- /bin/sh
```

The bare-verb form (`snapshot text`, `send \x01\\`, `resize 100 30`) is a
human-friendly alias for the same code, so a script and a test cannot drift from
what the API does.
