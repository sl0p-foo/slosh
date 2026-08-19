# contrib

Things that are useful but are not the multiplexer.

## themes/

Seven complete themes. Each sets **every** colour the config knows about, so
nothing silently falls back to a compiled-in default that belongs to a
different palette — there is a test that keeps them that way.

| | |
|---|---|
| `default` | what no config gets you: muted blue on neutral dark |
| `sl0p` | the house style: hot pink on near-black |
| `phosphor` | a green CRT that never quite went away |
| `amber` | the other CRT, for people who found green loud |
| `slate` | muted blues, for looking at all day |
| `paper` | a light theme, for a light terminal |
| `mono` | no colour at all, only weight and brightness |

To use one, put it in `~/.config/slosh/config.kdl` (or point
`$SLOSH_CONFIG` at it). A running session re-reads it the moment you save,
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

## slosh-dev

For working *on* slosh: build a new binary and pick it up without losing
your screen.

```sh
contrib/slosh-dev              # start, or reattach to, the dev session
contrib/slosh-dev restart      # from inside it, after a build
```

A running session keeps the binary it started with — `reload` re-reads the
config, it cannot re-read the code — so a new build needs a new server. The
only thing worth carrying across is the layout, and slosh can write its own:
`{"cmd":"dump-layout"}` is the inverse of `--layout`, so `restart` dumps,
quits, and the loop outside comes back with the same tabs, splits,
proportions, directories, and the same tab and pane focused.

What it cannot bring back is what was running *inside* a program. A shell
comes back as a shell, in the right directory, with an empty history. That is
the honest limit of restoring a layout rather than a session, and the script
says so rather than pretending.

`restart` has to be two halves for a reason worth knowing: it runs inside the
session, so it cannot do the restarting — the moment the session ends, so does
the shell it was typed in. It leaves the layout and a note; the loop, which is
outside, picks both up.

## shaders/ and shader-tour

Thirty-two ready-made shaders, one file each, generated from the presets in
`shadertoy.html` by `gen-shaders` — the page is where they are written and
previewed, and a second copy kept by hand would be a second copy kept badly.
`tests/test_shader_presets.py` fails if the two ever disagree.

To use one, append it to `~/.config/slosh/config.kdl`; a running session
picks it up when you save.

```sh
cat contrib/shaders/guides-torch.kdl >> ~/.config/slosh/config.kdl
```

To look before choosing:

```sh
contrib/shader-tour                 # spawn a session and cycle every one
contrib/shader-tour crt             # cycle one group
contrib/shader-tour torch           # apply one to the session named by $SESSION
DWELL=8 contrib/shader-tour motion  # linger longer on each
```

The groups are `guides` (a cursor line, a crosshair, indent guides, a torch —
things that tell you where you are), `crt` (phosphor, amber, film grain, a
rolling bar), `motion` (sonar, ripples, plasma, matrix rain), `built-ins`
(every compiled-in shader written as an expression, which is the point:
`vignette` is `dim` with an argument) and `pane-states`.

Move the mouse while the cursor-following ones are up. `curx`/`cury` is where
the cursor is, and standing still tells you nothing about a torch.

## shader-plugin/

A skeleton for adding your own shaders as a shared library, with a Makefile
and two worked examples (`checker`, `pulse`). Build it, drop the `.so` in
`~/.config/slosh/shaders/`, and name it in your config like a built-in.

The only header a plugin needs is `include/shader_abi.h`. What a shader may and
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

The presets are grouped, and most of them are less "effect" than
*instrument*: a cursor line, a crosshair, indent guides, a right margin, a
torch that dims everything more than eight cells from where you are looking
-- things that tell you where you are inside a program that has never heard
of any of them. Then a CRT section (phosphor, amber, film grain, a rolling
bar), a motion section (sonar pings from the cursor, ripples, matrix rain,
plasma), and every built-in written as an expression, which is the
equivalence the design rests on: `vignette` is `dim` with an argument.

It reimplements `src/expr.c` in JavaScript, which is the obvious hazard: a
preview that disagrees with the compiler is worse than no preview, and it
would disagree quietly. So `tests/test_shadertoy.py` lifts the evaluator *and
the presets* out of the page and runs them through both implementations --
new presets are checked without anyone remembering to add them. It also
evaluates every preset over a whole pane at four points in time and fails any
that computes zero everywhere, because an example that does nothing reads as
a broken shader system rather than a bad example. It found its first bug on
its first run.

## webdemo/

slosh in a browser tab, for people who want to try it without installing it:
a riscv64 Linux booted by Bellard's TinyEMU compiled to WebAssembly, an Alpine
userland we build into a disk image, and slosh as the thing you land in.

```sh
cd contrib/webdemo && make && make serve
```

The output is static files, so it can be served from anywhere that can serve a
directory. `make check` boots the same image in a native emulator and asserts
the session comes up, which is how a change to the guest is tested without
opening a browser at all.

## coverage

```sh
make coverage
```

Builds a second binary with gcc's `--coverage` into `build/cov`, runs every
suite against it (`$SLOSH_BIN` is how the same tests drive a different
build), and reads the counters back with gcov. Nothing is installed, the
normal zig build is untouched, and it takes about a minute.

It is a number to look at, not a number to chase. A line no test executes is
worth knowing about; a line executed by a test that asserts nothing is worth
nothing, and no coverage tool can tell those apart.

The C unit tests link the same instrumented objects, so `input.c`, `kdl.c`,
`shader.c` and `expr.c` get credit for the tests that actually exercise them —
without that they read as half-tested, which is a lie about where the tests
are rather than about the code.

## the agent skill

`.agents/skills/driving-slosh/SKILL.md` is the control socket written as
instructions rather than as a reference: which environment variables tell a
program in a pane which session it is in, why work belongs in a pane that was
*given* a command instead of typed into a shell, and that `purpose` is the handle
to find things by. It follows the `.agents/skills/<name>/SKILL.md` convention, so
an agent working in a checkout of this repo picks it up on its own; to use it
anywhere else, copy or symlink that directory into wherever your agent keeps
skills. It carries no paths from the machine it was written on, and
`tests/test_skill.py` keeps every verb it names honest.
