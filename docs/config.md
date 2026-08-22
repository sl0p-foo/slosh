# Configuration

One file, in [KDL](https://kdl.dev):

```
$SLOSH_CONFIG, else $XDG_CONFIG_HOME/slosh/config.kdl, else
~/.config/slosh/config.kdl
```

`C-a e` opens it in a pane, writing a starting file first if you have none.

**Saving it applies to every running session immediately.** A file that will not
parse is refused rather than half-applied, and the running config stays. Anything
smaller than that — an unknown shader, a binding that does not parse, an
[`include`](#include) that is not there — applies the rest of the file and says
which line it could not honour.

Three things are read later than the rest, because of *when* they are needed:
`shell` and the two `scrollback` limits apply to the next pane you open —
shrinking a pane's history retroactively would throw away output somebody is
reading — and a shader plugin that replaced one already loaded needs a new
session.

## Two files worth knowing

- **`slosh --dump-config`** writes every setting with its *default* value —
  not the value your config gives it. It is generated from the code, so it
  cannot drift, and it is a supported way to start a config:
  `slosh --dump-config > ~/.config/slosh/config.kdl`. To see what your own
  file does, `slosh --check` reads it and reports what it understood.
- **`config/config.kdl`**
  in the source tree is the same list with the *reasoning* attached — what each
  setting is for, what it cost to get right, and which ones are opinions. When
  the two disagree, the dump is right and that file is stale (a test says so).

## Checking one

```bash
$ slosh --check                     # the config a session would read
$ slosh --check themes/mine.kdl     # or one you have not installed yet
```

Every problem it found, one per line, with the file and line it happened at —
including a setting it does not know:

```
  cannot open /home/you/.config/slosh/themes/nope.kdl
  config.kdl:2: bad prefix: ctrl+nope
  config.kdl:3: bad key: wobble
  config.kdl:5: padding takes 1, 2 or 4 values (all, vertical horizontal, or top right bottom left), not 3
  config.kdl:6: unknown setting: wobble
  themes/mine.kdl:7: unknown shader: bloom
~/.config/slosh/config.kdl: 6 problems
```

The problems go to stderr and the summary with them. It exits 1 when it has
anything to say, so it drops into an editor's compile
step or a pre-commit hook with no glue. A clean run prints what it read — the
files, the prefix it ended up with, how many bindings — because otherwise a
config that was never opened and a config with nothing wrong look identical.

It is the loader, not a second implementation of it: the only checker that
cannot drift from what a session does is the one a session uses. The single
difference is how many problems each reports — a session has one status line and
shows the first, and a linter that stopped at the first mistake would make you
run it once per mistake.

## include

A config can be made of files:

```kdl
include "themes/amber.kdl"
include "keys/vim.kdl" "shaders/crt.kdl"
```

which is how you switch a theme by editing one line rather than pasting one in.
Nothing about it is theme-specific: an include is another config file applied
here, so the same line composes keys, shader chains, states or geometry.

- A relative path is relative to **the file doing the including**, never to the
  directory you started the session from. `~` is your home directory, an absolute
  path is itself, and an included file may include others.
- **What you include is the base**: the file with the `include` line in it wins
  wherever they disagree, and a later include wins over an earlier one. That
  holds wherever the line sits in the file.
- `keys` blocks add to what came before; a `shaders` or `states` block replaces
  the one it inherited.
- Every included file is watched, so saving the theme reloads the session too.

## Themes

Every colour the compositor draws has its own name under `theme { }` — 48 of
them, because six names doing thirty jobs meant the split guide could not be
recoloured without also recolouring the focused frame.

Six ready-made themes are in
`contrib/themes`:
`amber`, `mono`, `paper`, `phosphor`, `sl0p`, `slate` — plus `default`, the
compiled-in palette written out, so another theme is one include-swap away from
coming back. Include one and put your own two lines on top:

```kdl
include "~/.config/slosh/themes/phosphor.kdl"
theme { frame_focus "#00ff88" }     // ...but that one colour is mine
```

`contrib/theme-tour` cycles a running session through all of them.

## The knobs, briefly

| group | what is in it |
|---|---|
| geometry | `gap` `gap_aspect` `padding` `rounded` `title_align` `title_inset` `min_pane` `min_split` |
| chrome | `status_bar` `status_line` `status_pad` `hints` `version_banner` `pane_buttons` `bell_indicator` and the marks (`zoom_mark` `zoom_on_mark` `close_mark` `min_mark` `newtab_mark` `bell_mark`) |
| behaviour | `focus_follows_mouse` `scroll_lines` `scrollback` `scrollback_bytes` `toast_ms` `splash_ms` `hover_delay_ms` `double_click_ms` `word_separators` `anim_ms` `modal_scrim` `dim_unfocused` `float_shadow` `keep_dead` `in_band_shaders` `shell` `editor` `shader_dir` |
| projects | `project_roots` `project_layout` — see [workspaces](workspaces.md) |
| colour | `theme { }` |
| effects | `shaders { }`, `states { }` — see [shaders](shaders.md) and [chrome](chrome.md) |
| keys | `keys { }` — see [keys](keys.md) |

Two of those take more than one value. `gap` is in **rows**, and `gap_aspect`
says how many columns a row is worth (2 by default, because a cell is about
twice as tall as it is wide) — so both `gap` and `padding` are written in rows
and come out looking square.

`padding` is the space between a pane's frame and its contents, written the way
CSS does it:

```kdl
padding 1              // every side
padding 0 2            // vertical, horizontal
padding 2 0 0 1        // top, right, bottom, left
```

A number means the same thing however many of them you write, so `padding 1`,
`padding 1 1` and `padding 1 1 1 1` are one padding rather than three. Three
values is refused — CSS reads it as top/horizontal/bottom, and a line whose
meaning you have to look up is a line nobody can read.

## Scrollback

```kdl
scrollback 10000            // lines of history per pane; 0 keeps none
scrollback_bytes 16777216   // ...and the ceiling that count runs into
```

**The default was a library's, not a decision.** lib-vt is handed no limit and
picks its own — 10,000 *bytes*, which measures at 622 lines of an 80-column pane,
less than one `make` run. `scrollback` is that number replaced with the one every
other multiplexer settled on.

The two limits work together because either can be reached first and neither can
see what the other depends on: a line count cannot know how wide your terminal is
or how many styles a program used, and a byte count cannot know how many lines
that bought. Whichever bites first wins. Both are estimates — history is pruned a
page at a time (a page is about 400KB of grid), so a pane keeps a little more
than it was told, never less.

What the ceiling costs, measured on a pane filled with styled output:

| pane width | 10,000 lines costs | with the default ceiling |
|---|---|---|
| 80 columns | ~6 MB | every line |
| 200 columns | ~15 MB | about 2% fewer |
| 400 columns | ~32 MB | about half |

So it is invisible at ordinary widths and a brake at extraordinary ones, which is
its job. It is per **pane**: sixty panes full of history is sixty times it, which
is why there is no `unlimited` — at that many panes it would be a memory leak with
a friendly name. `scrollback_bytes 0` removes the ceiling if you have one very wide
pane and want every line of it.

Both apply to **the next pane you open**, not to panes already holding history:
shrinking one retroactively would throw away output somebody is reading.

## Pane states

A pane is tinted by what is *true* of it, which is the part you cannot discover
by looking:

```kdl
dim_unfocused 60          // the one knob most people want
float_shadow 110          // the shade a floating pane casts; 0 for none

states {                  // ...and the whole table underneath it
    dead { grayscale amount=200; dim amount=90 }
    suspended { grayscale amount=170; dim amount=60 }
    scrolled { tint amount=22 color="#7aa2f7" }   // follows theme's scroll_bg;
    // plus derived edge fades: the top and bottom rows melt towards an edge
    // while there is more content past it (`above` / `below` in expressions),
    // and turn solid at the end of the buffer -- nothing more to scroll to
    unfocused { dim amount=60 }    // writing this replaces dim_unfocused
    // floating { }   // a float; above unfocused, so floats are never dimmed
    // bell: a shimmer across the body (a soft sheen, gone within a second)
    // and a chrome pass on the frame -- blinks, then a breathe -- in theme's
    // bell colour, until the pane is looked at. See the chrome shaders page;
    // an empty bell { } removes it, bell_indicator false silences bell + mark.
}
```

The full ranking, most urgent first: `dragging`, `drop_hover`, `drop_target`,
`dead`, `suspended`, `bell`, `scrolled`, `floating`, `unfocused`. Exactly one wins — the
first that matches — because two reasons to be grey compound into one muddy grey
that reads as neither. Naming a state replaces its default outright, including
with nothing, which is how you turn one off.
