# Configuration

One file, in [KDL](https://kdl.dev):

```
$SL0PPTY_CONFIG, else $XDG_CONFIG_HOME/sl0ppty/config.kdl, else
~/.config/sl0ppty/config.kdl
```

`C-a e` opens it in a pane, writing a starting file first if you have none.

**Saving it applies to every running session immediately.** A file that will not
parse is refused rather than half-applied, and the running config stays. Anything
smaller than that — an unknown shader, a binding that does not parse, an
[`include`](#include) that is not there — applies the rest of the file and says
which line it could not honour.

Two settings are read later than the rest, because of *when* they are needed:
`shell` applies to the next pane you open, and a shader plugin that replaced one
already loaded needs a new session.

## Two files worth knowing

- **`sl0ppty --dump-config`** writes every setting with the value it currently
  has. It is generated from the code, so it cannot drift from the defaults, and
  it is a supported way to start a config:
  `sl0ppty --dump-config > ~/.config/sl0ppty/config.kdl`.
- **`config/config.kdl`**
  in the source tree is the same list with the *reasoning* attached — what each
  setting is for, what it cost to get right, and which ones are opinions. When
  the two disagree, the dump is right and that file is stale (a test says so).

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
`amber`, `mono`, `paper`, `phosphor`, `sl0p`, `slate`. Include one and put your
own two lines on top:

```kdl
include "~/.config/sl0ppty/themes/phosphor.kdl"
theme { frame_focus "#00ff88" }     // ...but that one colour is mine
```

`contrib/theme-tour` cycles a running session through all six.

## The knobs, briefly

| group | what is in it |
|---|---|
| geometry | `gap` `gap_aspect` `padding` `rounded` `title_align` `title_inset` `min_pane` `min_split` |
| chrome | `status_bar` `status_line` `status_pad` `hints` `version_banner` `pane_buttons` and the marks (`zoom_mark` `zoom_on_mark` `close_mark` `min_mark` `newtab_mark` `bell_mark`) |
| behaviour | `focus_follows_mouse` `scroll_lines` `toast_ms` `hover_delay_ms` `double_click_ms` `anim_ms` `modal_scrim` `dim_unfocused` `keep_dead` `shell` `editor` `shader_dir` |
| colour | `theme { }` |
| effects | `shaders { }`, `states { }` — see [shaders](shaders.md) and [chrome](chrome.md) |
| keys | `keys { }` — see [keys](keys.md) |

## Pane states

A pane is tinted by what is *true* of it, which is the part you cannot discover
by looking:

```kdl
dim_unfocused 60          // the one knob most people want

states {                  // ...and the whole table underneath it
    dead { grayscale amount=200; dim amount=90 }
    suspended { grayscale amount=170; dim amount=60 }
    scrolled { tint amount=22 color="#ff5fd7" }
    unfocused { dim amount=60 }    // writing this replaces dim_unfocused
}
```

The full ranking, most urgent first: `dragging`, `drop_hover`, `drop_target`,
`dead`, `suspended`, `bell`, `scrolled`, `unfocused`. Exactly one wins — the
first that matches — because two reasons to be grey compound into one muddy grey
that reads as neither. Naming a state replaces its default outright, including
with nothing, which is how you turn one off.
