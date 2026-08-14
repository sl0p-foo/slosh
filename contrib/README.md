# contrib

Things that are useful but are not the multiplexer.

## themes/

Six complete themes. Each sets **every** colour the config knows about, so
nothing silently falls back to a compiled-in default that belongs to a
different palette — there is a test that keeps them that way.

| | |
|---|---|
| `sl0p` | the default: hot pink on near-black |
| `phosphor` | a green CRT that never quite went away |
| `amber` | the other CRT, for people who found green loud |
| `slate` | muted blues, for looking at all day |
| `paper` | a light theme, for a light terminal |
| `mono` | no colour at all, only weight and brightness |

To use one, put it in `~/.config/sl0ppty/config.kdl` (or point
`$SL0PPTY_CONFIG` at it). A running session re-reads it the moment you save,
so you can edit and watch.

## theme-tour

```sh
contrib/theme-tour            # start a session and cycle every theme
contrib/theme-tour slate      # apply one to a running session
DWELL=8 contrib/theme-tour    # linger longer on each
SESSION=work contrib/theme-tour slate
```

It works by writing the theme over the file the session was started with and
letting the config watcher notice, which is also a fair demonstration of the
watcher.
