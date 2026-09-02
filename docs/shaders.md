# Shaders

slosh has the concept of `shaders`. shaders are (mathematical) functions applied
to the `cell`s of a pane. it runs after the pane has been composed and can be used
to visually alter the foreground and background colours (and other attributes) of
the final output that is send to the user's terminal. The actual text is never
altered by shaders, only the markup.

Here's a basic example of a shader chain setup:

```kdl
shaders {
    vignette amount=70
    ruler amount=60 at=80 color="#7aa2f7"
}
```

A chain is a sequence: `grayscale` then `tint` is not `tint` then `grayscale`.
Listing a shader twice is how you get two of it. An unknown name is refused with
a word and skipped, never guessed at.

## The built-in shaders

| shader      | takes                     | does                                           |
| ----------- | ------------------------- | ---------------------------------------------- |
| `dim`       | `amount`                  | darken towards black                           |
| `grayscale` | `amount`                  | drain colour, weighted for the eye             |
| `tint`      | `amount` `color=`         | pull towards a colour                          |
| `vignette`  | `amount`                  | darken towards the pane's edges                |
| `gradient`  | `amount` `direction=0..3` | fade towards the background                    |
| `zebra`     | `amount` `band=1`         | darken alternate bands; `band=1` is a scanline |
| `ruler`     | `amount` `at=80` `color=` | mark a column, background only                 |
| `margin`    | `amount` `at=100`         | everything past a column recedes               |
| `spotlight` | `amount` `radius=12`      | brightness falls away from the cursor          |

`amount` is `0..255` and defaults to `128`. The numbers beside the other properties are examples.

There are two special properties that belong to the _pass_ rather
than the shader and work on any of them:

- `where`: `where="content"|"chrome"` selects if the shader should apply to the chrome (border) of the pane or to the content (body) of the pane.
- `channel`: `channel="both"|"fg"|"bg"` selects if the shader should be applied to foreground, background or both.

## Strength as an expression

`amount` can be an expression instead of a number, evaluated for every cell.

```kdl
shaders {
    dim  amount="(y % 2) * 40"                        // scanlines
    dim  amount="(x > cols - 10) * 120"               // a right margin
    tint amount="255 - dist(x, y, curx, cury) * 12" color="#7aa2f7"
}
```

|           |                                                                                                             |
| --------- | ----------------------------------------------------------------------------------------------------------- |
| variables | `x` `y`: the cell, inside the rect the pass runs over                                                       |
|           | `cols` `rows`: the size of that rect                                                                        |
|           | `curx` `cury` `cursor`: the cursor, and whether this pane has it                                            |
|           | `focused`: 0 or 1                                                                                           |
|           | `t`: milliseconds, for animation                                                                            |
|           | `since`: milliseconds the pane has been in its current state                                                |
|           | `above` `below`: lines of scrollback hidden past the viewport's top and bottom edges, both 0 at the present |
| operators | `+ - * / %` (integer; division by zero is 0, not a crash)                                                   |
|           | `< > <= >= == != && \|\| !` give 0 or 1, so `(x < 10) * 200` is a rule                                      |
|           | `& \| ^ ~ << >>` on the 32-bit value                                                                        |
|           | `a ? b : c`: both sides are evaluated, then one is chosen                                                   |
| functions | `min(a,b)` `max(a,b)` `abs(a)` `clamp(v,lo,hi)`                                                             |
|           | `dist(x1,y1,x2,y2)`: counts a row double, because a cell is about twice as tall as it is wide               |
|           | `sin(deg)` `cos(deg)`: **degrees** in, −255..255 out, so `128 + sin(t / 4) / 2` is a breathe                |
| constants | `PI` = 180, `TAU` = 360: pi as an _angle_, which is what it is here: a half turn                            |

There are no loops and no recursion, on purpose: a config cannot spin and there
is nothing to sandbox. An expression that does not compile drops that one shader
with a warning; never the config, and never a half-strength version of what you
asked for.

The expression produces the **strength**, never the colour. That keeps the mixing
in C, gives an expression no way to produce an invalid cell.

## Ready-made

Thirty-two presets are in
`contrib/shaders`:
a cursor line, a crosshair, a torch, a phosphor CRT, sonar pings that follow
your cursor. Each is a file you can paste into your config or `include`. These
are some over the top examples, of course. But it's a good showcase of what
you can do with the slosh shaders!

## Prototyping in a pane

To allow for more rapid prototyping of shaders in live slosh session we have introduced
some ANSI escape sequences (OSC) to directly configurable shaders by sending terminal
sequences. Of course this is **not** enabled by default as we don't want random software
to start reconfiguring our shaders. enable `in_band_shaders` for this functionality.

```bash
printf '\033]5577;1;shader;chrome;tint color="#7aa2f7" amount="abs(t / 8 %% 510 - 255)"\033\\'
printf '\033]5577;1;shader;content;dim amount=90\033\\'
printf '\033]5577;1;shader;chrome;\033\\'   # that rect: back to normal
printf '\033]5577;1;shader;\033\\'          # no rect named, so both of them
```

The field after `shader` is which rect an entry should be appliedi and the rest of
the payload is in the config's syntax: one entry, several separated by `;`, or a whole `shaders { }` block.
The session answers on the program's stdin, `\033]5577;1;shader-reply;ok\033\\` or
`shader-reply;error;bad amount for tint: ...`, so a typo says so instead of looking like a shader that does nothing.

**Three ways to take it all back**, because the program that painted a pane is
not always in a state to put it back:

|                                                              |                                                                  |
| ------------------------------------------------------------ | ---------------------------------------------------------------- |
| `printf '\033]5577;1;shader;\033\\'`                         | from the program, one exchange, both rects                       |
| `printf '\033]5577;1;shader-load;/path/to/preset.kdl\033\\'` | a `shaders { }` file, read by the session and routed by `where=` |
| `slosh cmd '{"cmd":"clear-shaders","id":3}'`                 | from outside; `id` 0 or absent means the focused pane            |
| the `clear-shaders` action                                   | on a key you bind, or from the palette                           |

`contrib/shader-repl` is that loop with a prompt on it, and **what you type at it
is what a config file says**, not a dialect of it:

```
chrome> tint amount=200                       one entry, for the rect the prompt names
chrome> tint where="content" amount=200       ...or the one the entry names, which wins
chrome> dim amount=90; tint amount=40         several, separated as a config separates them
chrome> shaders { … }                         the block itself, over as many lines as it takes
chrome> include "contrib/chrome/shine.kdl"    a file, the way a config includes one
chrome> :load shine                           the same, for the presets that ship with it
```

`:paste` prints the document back as a `shaders { }` block with `where=` on
every entry, and that block can be typed straight back in, which is the point
of borrowing the syntax rather than inventing one. The reply counts
(`1 chrome, 1 content`) say where the passes went, which is the only way to see
that an entry's `where=` did what you meant.

**Off by default.** It needs `in_band_shaders true`.

## Your own, compiled

A shader is a C function from one cell to that cell's colours. Any `*.so` in
`~/.config/slosh/shaders/` that exports one is loaded at startup and named in
the config exactly like a built-in. Skeleton, Makefile and the rules a shader
has to keep:
`contrib/shader-plugin`.

It is native code in the session's process, so install ones you trust. It has
the same standing as `shell` and a layout's `command=`, which can already run
anything as you.
