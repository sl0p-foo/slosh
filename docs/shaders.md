# Shaders

A colour pass over a pane's cells: a pure function from (cell, position,
strength) to that cell's colours. It runs after the pane's contents are composed
and before the chrome that goes over them, which is why "contents, not chrome"
falls out of the paint order rather than needing a rule. For the frame, see
[chrome shaders](chrome.md).

Cells only — foreground, background, attributes. Text and width are never
touched, because rewriting text would desync selection and copy.

```kdl
shaders {                       // every pane gets these, in the order written
    vignette amount=70
    ruler amount=60 at=80 color="#ff5fd7"
}
```

A chain is a sequence: `grayscale` then `tint` is not `tint` then `grayscale`.
Listing a shader twice is how you get two of it. An unknown name is refused with
a word and skipped, never guessed at.

## The built-ins

| shader | takes | does |
|---|---|---|
| `dim` | `amount` | darken towards black |
| `grayscale` | `amount` | drain colour, weighted for the eye |
| `tint` | `amount` `color=` | pull towards a colour |
| `vignette` | `amount` | darken towards the pane's edges |
| `gradient` | `amount` `direction=0..3` | fade towards the background |
| `zebra` | `amount` `band=1` | darken alternate bands; `band=1` is a scanline |
| `ruler` | `amount` `at=80` `color=` | mark a column, background only |
| `margin` | `amount` `at=100` | everything past a column recedes |
| `spotlight` | `amount` `radius=12` | brightness falls away from the cursor |

`amount` is 0..255 and defaults to 128. Two props belong to the *pass* rather
than the shader and work on any of them: `where="content"|"chrome"` and
`channel="both"|"fg"|"bg"`.

## Strength as an expression

`amount` can be an expression instead of a number, evaluated for every cell.
That is how you get an effect nobody built in, without building anything:

```kdl
shaders {
    dim  amount="(y % 2) * 40"                        // scanlines
    dim  amount="(x > cols - 10) * 120"               // a right margin
    tint amount="255 - dist(x, y, curx, cury) * 12" color="#ff5fd7"
}
```

| | |
|---|---|
| variables | `x` `y` — the cell, inside the rect the pass runs over |
| | `cols` `rows` — the size of that rect |
| | `curx` `cury` `cursor` — the cursor, and whether this pane has it |
| | `focused` — 0 or 1 |
| | `t` — milliseconds, for animation |
| | `since` — milliseconds the pane has been in its current state |
| operators | `+ - * / %` (integer; division by zero is 0, not a crash) |
| | `< > <= >= == != && \|\| !` give 0 or 1, so `(x < 10) * 200` is a rule |
| | `a ? b : c` — both sides are evaluated, then one is chosen |
| functions | `min(a,b)` `max(a,b)` `abs(a)` `clamp(v,lo,hi)` |
| | `dist(x1,y1,x2,y2)` — counts a row double, because a cell is about twice as tall as it is wide |

There are no loops and no recursion, on purpose: a config cannot spin and there
is nothing to sandbox. An expression that does not compile drops that one shader
with a warning — never the config, and never a half-strength version of what you
asked for.

The expression produces the **strength**, never the colour. That keeps the mixing
in C, gives an expression no way to produce an invalid cell, and makes the result
cacheable: a program that does not read the clock is evaluated once per cell into
a map and reused, so a shader you wrote in your config costs about what a
compiled one does. Reading `t` or `since` costs a per-cell evaluation every
frame, which is the honest price of animation.

## Ready-made

Thirty-two presets are in
`contrib/shaders` —
a cursor line, a crosshair, a torch, a phosphor CRT, sonar pings that follow
your cursor. Each is a file you can paste into your config or `include`.

`contrib/shader-tour` cycles a running session through them, and
`contrib/shadertoy.html` previews the language in a browser (a test cross-checks
that preview against the real compiler, so it cannot lie to you).

## Your own, compiled

A shader is a C function from one cell to that cell's colours. Any `*.so` in
`~/.config/sl0ppty/shaders/` that exports one is loaded at startup and named in
the config exactly like a built-in. Skeleton, Makefile and the rules a shader
has to keep:
`contrib/shader-plugin`.

It is native code in the session's process, so install ones you trust — the same
standing as `shell` and a layout's `command=`, which can already run anything as
you.
