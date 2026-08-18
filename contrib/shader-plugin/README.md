# Shader plugins

A shader is a C function from one cell to that cell's colours. The built-in
ones live in the binary; you can add your own as a shared library, without
rebuilding slosh.

```bash
make            # builds example.so
make install    # copies it to ~/.config/slosh/shaders/
```

Then name it in your config exactly as you would a built-in:

```kdl
shaders {
    checker amount=40 band=4
    pulse   amount=60 color="#ff5fd7"
}

states {
    unfocused { checker amount=25 band=6 }
}
```

Start a new session to pick it up. A running session will load plugins that
have been **added** when you save your config, but never reloads or unloads
one that is already in — a shader's function pointer may be sitting in a live
config or on a pane, and unmapping it underneath would be a crash. To replace
a plugin, restart the session.

## Writing one

Everything you need is in [`src/shader_abi.h`](../../src/shader_abi.h) — one
header, no libghostty-vt, no other slosh types. `example.c` is a working
plugin with two effects and comments about what each one demonstrates.

The shape is:

```c
#include "shader_abi.h"

static void sh_mine(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
    /* sh->color, sh->amount, sh->param  — what the config set
       ctx->x, ctx->y, ctx->cols, ctx->rows, ctx->now_ms, ctx->focused,
       ctx->has_cursor, ctx->cursor_x, ctx->cursor_y, ctx->default_fg/bg
       c->fg, c->bg, c->attrs — yours to write */
}

static const shader_def_t SHADERS[] = {{"mine", sh_mine}};
SLOSH_SHADER_PLUGIN("my-shaders", SHADERS)
```

Three rules, which are also why shaders can be this simple:

- **Colours only.** Write `c->fg`, `c->bg`, `c->attrs`. Never `c->text`,
  `c->len` or `c->width`: selection and copy read the terminal rather than the
  screen, so rewriting text there desyncs them, and changing a cell's width
  breaks an invariant the compositor relies on.
- **One cell.** No neighbours, no history. That is what makes a shader a pure
  function of position and colour — and what makes blurs impossible, which is
  the price of needing no second buffer.
- **Fast and total.** This runs per cell, per pass, per frame — tens of
  thousands of calls at up to 120Hz. Do not allocate, block, do I/O, or loop
  for a length you do not control. Integer maths is plenty: the output is
  three bytes.

## Config parameters

Each shader node takes the same three, and what they mean is yours to decide:

| in the config | in the struct | |
|---|---|---|
| `color="#rrggbb"` | `sh->color` | defaults to the theme's `frame_focus` |
| `amount=0..255` | `sh->amount` | defaults to 128; 0 should be identity |
| `at=` `radius=` `band=` `direction=` | `sh->param` | one number, four spellings |

The four spellings are one field. `at` for a column, `radius` for a distance,
`band` for a size, `direction` for a direction — whichever reads right for
your effect.

## Do you actually need one?

A shader's `amount` can be an expression in the config
(`dim amount="(y % 2) * 40"`), evaluated per cell and about as fast as
compiled code. That covers every effect that is "a strength computed from
where the cell is", which is most of them, with no compiler and no `.so`.

Write a plugin when the *colour maths* is the point rather than the strength
-- a palette mapping, a channel swap, a curve -- or when you want an effect
under a name of its own.

## Compatibility

The plugin declares which ABI it was built against, and the sizes of the three
structs it shares with slosh. A mismatch is refused at load with a line on
stderr rather than trusted into a crash, so a plugin left over from an older
version fails safely and everything else still loads.

**The ABI is currently 2.** It went to 2 when `shader_t` gained the compiled
expression that backs `amount`; a plugin built against 1 is refused with
`shader ABI 1, we speak 2` and needs nothing but a rebuild.

## Trust

A shader plugin is native code loaded into the process that holds all your
terminals. It runs with your privileges and there is no sandbox. That is why
the search path is a directory you own and not a config value pointing
anywhere: installing a plugin is a decision, and it should look like one.
