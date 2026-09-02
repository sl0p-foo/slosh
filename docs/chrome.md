# Chrome shaders

A frame is a rect too, so it is a pass too. `where="chrome"` on any entry in
`shaders` or in a `states` block runs that pass over a pane's **frame** (border,
padding, title, buttons) instead of its contents.

```kdl
states {
    // a bell nobody has answered: a quarter-second flash, then gone
    bell { tint where="chrome" channel="fg" color="#ffcc00" amount="(since < 250) * 255" }
}
```

Nothing else changes: same shaders, same `amount`, same
[expressions](shaders.md#strength-as-an-expression), same chain order. One new
word.

## Positions are the whole frame's

`x`/`y` are counted from the frame's top-left corner, with `cols`/`rows` the size
of the pane's rect, not of one side. One pass over the whole ring, so an effect
can travel round a border instead of restarting at every corner.

For anything that _travels_, though, the coordinate you want is distance along
the ring, which the language can work out. Clockwise from the top-left:

```
(y == 0 ? x : x == cols-1 ? cols - 1 + y
 : y == rows-1 ? 2 * cols + rows - 3 - x : 2 * cols + 2 * rows - 4 - y)
```

There is no way to name a subexpression, so the formula is written out in full,
which is why a small effect is a long line.

Thirteen of them are ready to paste in
`contrib/chrome`:
a shine running round the border, a sine comet with softer shoulders, marching
ants, a standing wave, drifting sparkles, ripples leaving a corner in both
directions, etc.

## A border that says something

Depending on a pane's [state](config.md#pane-states), we can visually communicate
through it's chrome:

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

`since` is what makes a _flash_ possible: it is how long the pane has been in
this state, where `t` only knows what time it is. The `bell` state ends when you
look at the pane, because that is what answering a bell is. :-)

## Animation costs a clock

Anything that reads `t` or `since` asks the session to keep painting, at
`anim_ms` (50ms, so 20fps) and only while such a pass is actually on screen.

`anim_ms 0` turns the clock off, which leaves an animated shader advancing only
when something else causes a frame.
