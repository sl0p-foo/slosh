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

## shader-plugin/

A skeleton for adding your own shaders as a shared library, with a Makefile
and two worked examples (`checker`, `pulse`). Build it, drop the `.so` in
`~/.config/sl0ppty/shaders/`, and name it in your config like a built-in.

The only header a plugin needs is `src/shader_abi.h`. What a shader may and
may not do — and why a plugin is native code rather than a sandboxed one —
is in that directory's README.

## shadertoy.html

Write a shader expression, watch it happen. One file, no build, no server:
open it in a browser and it works, including offline.

The point of previewing an expression is that you cannot see one by reading
it -- `(x > cols - 10) * 120` is a right margin and `(y % 2) * 40` is
scanlines, and neither of those is obvious until it is on a screen. Move the
mouse over the preview to move the cursor, so `dist(x, y, curx, cury)` does
what it will do in a pane. Anything reading `t` animates. Presets cover every
built-in written as an expression, which is the equivalence the design rests
on, and the config line to paste is printed underneath.

It reimplements `src/expr.c` in JavaScript, which is the obvious hazard: a
preview that disagrees with the compiler is worse than no preview, and it
would disagree quietly. So `tests/test_shadertoy.py` lifts the evaluator out
of the page, runs a few hundred expression/environment pairs through both it
and the real compiler, and fails if a single number differs. It found its
first bug on its first run.
