# Chrome shaders

A frame is a rect too, so it is a pass too. `where="chrome"` on any entry in
`shaders` or in a `states` block runs that pass over a pane's **frame** — border,
padding, title, buttons — instead of its contents.

```kdl
states {
    // a bell nobody has answered: a quarter-second flash, then gone
    bell { tint where="chrome" channel="fg" color="#ffcc00" amount="(since < 250) * 255" }
}
```

Nothing else changes: same shaders, same `amount`, same
[expressions](shaders.md#strength-as-an-expression), same chain order. One new
word.

## Two rules

**`channel="fg"`, almost always.** A border's *background* is the terminal's own
default; mixing that towards a colour paints a rectangle behind the glyphs
instead of colouring them, and leaves it there until the pane changes state.
`fg` also keeps the padding inside the border out of it, since a blank cell has
no foreground to colour — so the effect lands on exactly the rules, the title and
the buttons.

**The contents are cut out of the pass.** A full-strength chrome tint cannot
reach your text, and a full-strength content tint cannot reach the border. They
are two rects and two passes.

## Positions are the whole frame's

`x`/`y` are counted from the frame's top-left corner, with `cols`/`rows` the size
of the pane's rect — not of one side. One pass over the whole ring, so an effect
can travel round a border instead of restarting at every corner.

For anything that *travels*, though, the coordinate you want is distance along
the ring, which the language can work out — clockwise from the top-left:

```
(y == 0 ? x : x == cols-1 ? cols - 1 + y
 : y == rows-1 ? 2 * cols + rows - 3 - x : 2 * cols + 2 * rows - 4 - y)
```

with the perimeter itself `2 * cols + 2 * rows - 4`. Two things fall out of it:

- `min(d, P - d)` is distance the short way round, so an effect crosses the
  corner it started from instead of stopping at a seam.
- `(p - head + P) % P` is "how far ahead of the head", which makes an arc rotate
  rather than a blob slide.

There is no way to name a subexpression, so the formula is written out in full —
which is why a small effect is a long line.

Seven of them are ready to paste in
`contrib/chrome`:
a shine running round the border, drifting sparkles, ripples leaving a corner in
both directions, a rotating arc, a static gradient, corner glints, and a sheen
across the title rule.

## A border that says something

Hung off a [state](config.md#pane-states), this is chrome doing the job chrome is
for, in no room and no words:

```kdl
states {
    // a dead pane's frame breathes red until you deal with it
    dead { tint where="chrome" channel="fg" color="#ff0033" amount="abs(t/8%510-255)" }
    // one that has not started yet is outlined, not filled
    suspended { tint where="chrome" channel="fg" color="#8a8a95" amount=160 }
    // and the panes you are not in recede, frame and contents together
    unfocused { dim amount=60; dim where="chrome" channel="fg" amount=90 }
}
```

`since` is what makes a *flash* possible: it is how long the pane has been in
this state, where `t` only knows what time it is. The `bell` state ends when you
look at the pane, because that is what answering a bell is.

A pane that a small terminal has collapsed to a single row is chrome all the way
through, which makes this the only kind of pass that can still colour it — and
that is the state where a colour is worth the most.

## Animation costs a clock

Anything that reads `t` or `since` asks the session to keep painting, at
`anim_ms` (50ms, so 20fps) and only while such a pass is actually on screen. A
pulse hung off `dead` costs a frame clock while a pane is dead and nothing for
the rest of the session; the same pass in the global `shaders` block costs it for
the whole session.

`anim_ms 0` turns the clock off, which leaves an animated shader advancing only
when something else causes a frame.
