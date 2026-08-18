# chrome presets

Colour passes over a pane's **frame** rather than its contents — `where="chrome"`
— each in its own file, ready to paste into `~/.config/slosh/config.kdl`.

```sh
cat contrib/chrome/shine.kdl >> ~/.config/slosh/config.kdl   # then save
```

Hand-written, unlike [`contrib/shaders`](../shaders), which is generated from
`contrib/shadertoy.html`: the toy previews a pane's contents, and a frame is a
different rect with no preview to give. So these are checked the only way that
means anything — `tests/test_chrome_presets.py` loads every file here into a
real session and asserts that the frame moves and the contents do not.

Try one without editing anything: run [`contrib/shader-repl`](../shader-repl) in a
pane and type `:load sine-comet`. It applies to that pane only, `:paste` gives you
the `include` line to keep, and `:both` puts the pane back.

| file | what it does |
|---|---|
| `shine.kdl` | a highlight running clockwise round the border, wrapping at the corner |
| `shimmer.kdl` | sparkles drifting along it |
| `ripple.kdl` | two pulses leaving the top-left corner and meeting at the far side |
| `spinner.kdl` | a fixed arc rotating — work in progress, for a state |
| `gradient.kdl` | not animated: lit from the top-left, so the frame reads as a surface |
| `corner-glints.kdl` | the four corners breathing together |
| `title-sheen.kdl` | the top rule only, where the title sits |
| `sine-comet.kdl` | an arc gliding round with a sine's shoulders, not a ramp's corner |
| `marching-ants.kdl` | two on, two off, stepping round — the selection marquee |
| `standing-wave.kdl` | three lobes that do not travel; the pattern breathes in place |
| `bit-drift.kdl` | position XOR clock, three bits — a machine thinking |
| `heartbeat.kdl` | lub-dub, then a rest. For a state |
| `xor-barcode.kdl` | the top rule as a barcode, sliding a character at a time |

## the perimeter trick

On a frame, `x`/`y` are rarely what you want. An effect that travels round a
border wants **distance along the ring**, and the language can work it out —
clockwise from the top-left corner, in cells:

```
(y == 0        ? x
 : x == cols-1 ? cols - 1 + y
 : y == rows-1 ? 2 * cols + rows - 3 - x
 :               2 * cols + 2 * rows - 4 - y)
```

The perimeter itself is `2 * cols + 2 * rows - 4`. Two things fall out of it:

- **wrapping.** `min(d, P - d)` is distance the short way round, so an effect
  crosses the corner it started from instead of stopping at a seam.
- **direction.** `(p - head + P) % P` is "how far ahead of the head", which is
  what makes an arc rotate rather than a blob slide.

There is no way to name a subexpression, so the formula is written out in full
in each file. That is the ugly part of these, and the reason the files are long
when the effects are small.

## the angle idiom

`sin` and `cos` take **degrees**, and `PI` is 180 because pi in an angle language
is a half turn. So `TAU * p / perim` is "this cell's position, as an angle round
one lap", which is the line under most of the newer presets:

- `sin(TAU * p / perim)` — one cycle over the whole ring, and it wraps at the
  corner for free, because so does a sine.
- `sin(TAU * p * 3 / perim)` — three lobes. Any whole number fits the ring
  exactly, which is what keeps a standing wave standing.
- `* 14 - 3000` after it keeps only the top of the crest: an arc rather than a
  glow over everything. Overdrive then clip is how you sharpen anything here.

A sine costs a table lookup and no more than the triangle wave
`abs(t / 6 % 510 - 255)` it replaces — but it has no corner in it, which is the
difference between a frame that pulses and one that flickers.

## two rules worth knowing

**`channel="fg"`, always, on a frame.** A border's *background* is the
terminal's own default; mixing that towards a colour paints a rectangle behind
the glyphs instead of colouring them, and leaves it there. `fg` also keeps the
padding inside the border out of it, since a blank cell has no foreground to
colour — so the effect lands on exactly the rules, the title and the buttons.

**Anything that reads `t` asks the session to keep painting** (`anim_ms`, 20fps
by default) for as long as the pass is running. On a state that is the point: a
pulse hung off `bell` or `dead` costs a frame clock while a pane is ringing or
finished, and nothing for the rest of the session. Hung off every pane, as these
files do for previewing, it costs that clock for the whole session — which is
fine for a laptop and worth knowing about on a battery.

`gradient.kdl` reads no clock at all: it is computed once into a per-cell map
and reused, so it costs nothing per frame.

## states

Any of these is a state chain away from meaning something rather than just
looking like something:

```kdl
states {
    // the arc turns while a pane has not been started yet
    suspended { tint where="chrome" channel="fg" color="#ffcc00" amount="…spinner…" }
    // and a bell nobody has answered flashes, then settles
    bell { tint where="chrome" channel="fg" color="#ffcc00" amount="(since < 250) * 255" }
}
```

`since` is how long the pane has been in its current state, which is what makes
a one-shot effect possible: `t` says what time it is, not how long ago something
happened. The full list of states, and what each one means, is in
`config/config.kdl`.
