# Configuration

One file, in [KDL](https://kdl.dev):

```
$SLOSH_CONFIG, else $XDG_CONFIG_HOME/slosh/config.kdl, else
~/.config/slosh/config.kdl
```

`C-a e` (`edit_config`) opens it in a pane, writing a starting file first if you have none.

**Saving it applies to every running session immediately.**

Some tips:

- **`slosh --dump-config`** writes every setting with its _default_ value,
  not the value your config gives it. It is generated from the code, so it
  cannot drift, and it is a supported way to start a config:
  `slosh --dump-config > ~/.config/slosh/config.kdl`.
- To see what your own file does, `slosh --check` reads it and reports what it understood.

## Sanity checking your slosh config

```bash
$ slosh --check                     # the config a session would read
$ slosh --check themes/mine.kdl     # or one you have not installed yet
```

Every problem it found, one per line, with the file and line it happened at,
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
step or a pre-commit hook with no glue. A clean run prints what it read (the
files, the prefix it ended up with, how many bindings).

## include

A config can be composed of multiple files:

```kdl
include "themes/amber.kdl"
include "keys/vim.kdl" "shaders/crt.kdl"
```

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

Every colour the compositor draws has its own name under `theme { }`.

Six ready-made themes are in
`contrib/themes`:
`amber`, `mono`, `paper`, `phosphor`, `sl0p`, `slate`, plus `default`, the
compiled-in palette written out, so another theme is one include-swap away from
coming back. Include one and put your own two lines on top:

```kdl
include "~/.config/slosh/themes/phosphor.kdl"
theme { frame_focus "#00ff88" }     // ...but that one colour is mine
```

## The knobs, briefly

| group     | what is in it                                                                                                                                                                                                                                                                      |
| --------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| geometry  | `gap` `gap_aspect` `padding` `compact` `rounded` `title_align` `title_inset` `min_pane` `min_split`                                                                                                                                                                                |
| chrome    | `status_bar` `status_line` `status_pad` `hints` `version_banner` `pane_buttons` `bell_indicator` and the marks (`zoom_mark` `zoom_on_mark` `close_mark` `min_mark` `newtab_mark` `bell_mark`)                                                                                      |
| behaviour | `focus_follows_mouse` `ctrl_d_exits` `scroll_lines` `scrollback` `scrollback_bytes` `toast_ms` `splash_ms` `hover_delay_ms` `double_click_ms` `word_separators` `anim_ms` `modal_scrim` `dim_unfocused` `float_shadow` `keep_dead` `in_band_shaders` `multi_attach` `attach_indicator` `size_follows` `shell` `editor` |
| projects  | `project_roots` `project_layout` (see [workspaces](workspaces.md))                                                                                                                                                                                                                 |
| colour    | `theme { }`                                                                                                                                                                                                                                                                        |
| effects   | `shaders { }`, `states { }` (see [shaders](shaders.md) and [chrome](chrome.md))                                                                                                                                                                                                    |
| keys      | `keys { }` (see [keys](keys.md))                                                                                                                                                                                                                                                   |

A few of those take more than one value. `gap` is in **rows**, and `gap_aspect`
says how many columns a row is worth (2 by default, because a cell is about
twice as tall as it is wide), so both `gap` and `padding` are written in rows
and come out looking square. `min_pane cols=24 rows=6` and
`min_split cols=32 rows=8` name their two floors as properties (what each floor
_means_ is on the [panes page](panes.md#small-terminals)), and `project_roots`
takes one path per argument; see [workspaces](workspaces.md).

`padding` is the space between a pane's frame and its contents, written the way
CSS does it:

```kdl
padding 1              // every side
padding 0 2            // vertical, horizontal
padding 2 0 0 1        // top, right, bottom, left
```

Three values is refused: CSS reads it as top/horizontal/bottom, and a line
whose meaning you have to look up is a line nobody can read.

## Compact

```kdl
compact true
```

Shared borders instead of gaps.

```
╭────── nvim ───── ▬ □ ✕ ─┬── npm run dev ── ▬ □ ✕ ─╮
│                         │                         │
│                         ├────── shell ──── ▬ □ ✕ ─┤
│                         │                         │
╰─────────────────────────┴─────────────────────────╯
```

What changes, and what does not:

- `gap` stops applying.
- **Dividers drag exactly as gaps did**, corners included, and the same hover
  hints appear on them. A pane whose title line is a shared divider is still
  dragged, by its name.
- **Interior edges give up click-to-split**: a one-cell line cannot honestly
  hold both verbs, and the whole line is the resize handle. The outer frame
  keeps its split handles, and the keyboard splits anything, as ever.
- **Floats keep the classic frame** (an overlay needs its own edge), and so
  does a zoomed pane or a flattened tab: nothing there is packed against
  anything.
- Chrome shaders run over the pane's stretch of the shared lines; a line
  between two panes belongs to both, which is what sharing means.

## Scrollback

```kdl
scrollback 10000            // lines of history per pane; 0 keeps none
scrollback_bytes 16777216   // .. and the byte ceiling
```

## Pane states

A pane can be tinted depending on what "state" it is in. This way you easily
make different states visually apparent.

For common tinting configuration we have some short hands:

```kdl
dim_unfocused 60          // the one knob most people want
float_shadow 110          // the shade a floating pane casts; 0 for none
```

But they can also be more fine grainly configured:

```kdl
states {                  // ...and the whole table underneath it
    dead { grayscale amount=200; dim amount=90 }
    suspended { grayscale amount=170; dim amount=60 }
    scrolled { tint amount=22 color="#7aa2f7" }   // follows theme's scroll_bg;
    unfocused { dim amount=60 }    // writing this replaces dim_unfocused
    // floating { }   // a float; above unfocused, so floats are never dimmed
}
```

The full ranking, most urgent first:

- `dragging`
- `drop_hover`
- `drop_target`,
- `dead`
- `suspended`
- `bell`
- `scrolled`
- `floating`
- `unfocused`

Exactly one wins, the first that matches.
