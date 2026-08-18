# Keys

Everything is the **leader** (`C-a` by default) and then a key. Pressing the
leader twice sends it to the program in the pane.

`C-a ?` shows a cheatsheet built from the bindings your config actually has, so
it cannot disagree with your keyboard. `C-a p` opens the same list as a palette
you can type into.

On a terminal too short for the whole sheet it shows what fits and says how much
it did not — `+7 more, in docs/keys.md`, which is the table below.

## Defaults

| key | does |
|---|---|
| `Enter` | split whichever way there is more room — across the longer side, and the other axis if that will not fit |
| `\` `-` | split into columns / rows, when you mean one |
| `h` `j` `k` `l`, or arrows | move focus |
| `o` | the next pane |
| `H` `J` `K` `L`, or shift+arrows | move the boundary between panes |
| `=` | give every visible pane an even share |
| `Space` | turn the layout a quarter turn (four brings it back) |
| `z` | zoom this pane to fill the tab, and back |
| `m` | minimise it into the strip along the bottom |
| `x` | close this pane |
| `X` | close this tab, and everything in it |
| `r` | run a finished pane's command again |
| `P` | set this pane's [purpose](layouts.md#purposes) |
| `>` `<` | push this pane to the next / previous tab (a toast says where it went) |
| `b` | break this pane out into a tab of its own |
| `c` | new tab |
| `Tab` `shift+Tab` | next / previous tab |
| `1`…`9` | go to that tab |
| `f` | find a pane by name |
| `w` | the projects picker — every project under your roots, open or not ([workspaces](workspaces.md)) |
| `W` | write this tab out as this project's layout ([workspaces](workspaces.md)) |
| `p` | the command palette |
| `PgUp` `PgDn` `Home` `End` | [scrollback](config.md#scrollback) (the wheel works too) |
| `e` | edit the config, in a pane |
| `?` | this list |
| `d` | detach, leave it running |
| `q` | quit the session |
| `C-a` | send a literal `C-a` |

**`x`/`X`, `p`/`P` and `w`/`W` are deliberately shifted pairs on one letter.**
Close this pane / close this tab; run a command / tag this pane; go to a project /
write this project down. The shifted half is the same verb on a bigger thing, or
the thing you reach for within a moment of the other, so the shift is less to
remember than a second letter would have been.

`X` says how many panes went with the tab, since most of them were not the one you
were looking at. On the **last** tab it refuses and points at `q` instead: a key
that closes a tab four times and ends your session the fifth is one you cannot
press without counting first.

## Rebinding

`C-a` is a default, not a decision. If you want it back for start-of-line — and
plenty of people do — one line takes it:

```kdl
keys { prefix "ctrl+b" }        // or ctrl+space, or alt+x, or ...
```

Everything that names the prefix follows what you bound: the badge in the status
bar, the cheatsheet's `C-a then:` caption, the palette's key column.

A `keys` block **adds to** the defaults rather than replacing them, so binding
one key leaves the rest alone. `"none"` takes a binding away:

```kdl
keys {
    bind "v" "split-cols"       // as well as \
    bind "-" "none"             // and no more splitting into rows
}
```

Modifiers are `ctrl+ alt+ shift+ super+`, or the shorthand the cheatsheet
prints: `C-` `M-` `S-`. Named keys are `left right up down enter tab escape
space backspace home end pageup pagedown delete insert backslash minus slash
comma period`, and the arrows can be written as `←` `→` `↑` `↓` too — so a chord
copied off the cheatsheet is a chord you can paste into a config.

Anything else is the character you press. That includes the shifted ones, which
carry their own shift: `"?"` is `"shift+slash"`, `"H"` is `"shift+h"`, `"|"` is
`"shift+backslash"`. A punctuation binding written that way answers both
encodings, because whether your terminal reports `?` with a shift modifier or as
a bare byte is up to your terminal and not to what you meant.

`slosh --check` reads a config and says what it could not honour, one problem
per line with a file and a line number. Worth running after editing bindings by
hand.

## Without the leader

Bindings in a `direct` block fire the moment you press them:

```kdl
keys {
    direct {
        bind "ctrl+alt+left" "focus-left"
        bind "ctrl+alt+right" "focus-right"
    }
}
```

That chord is then gone from every program in every pane, because it has to be.
It is a footgun and it is yours; the cheatsheet lists direct bindings apart,
under "without the leader", so the `C-a then:` caption above stays true.

## Every action

Any of these can be bound, and any of them can be run from the palette without
being bound at all.

| group | actions |
|---|---|
| panes | `split` `split-cols` `split-rows` `close-pane` `rerun` `zoom` `minimize` `rotate-layout` `clear-shaders` `pane-to-next-tab` `pane-to-prev-tab` `pane-to-new-tab` `set-purpose` |
| focus | `focus-left` `focus-right` `focus-up` `focus-down` `focus-next` `finder` |
| size | `resize-left` `resize-right` `resize-up` `resize-down` `equalize` |
| tabs | `new-tab` `close-tab` `next-tab` `prev-tab` `select-tab-1` … `select-tab-9` |
| projects | `workspaces` `save-workspace` |
| scroll | `scroll-up` `scroll-down` `scroll-page-up` `scroll-page-down` `scroll-top` `scroll-bottom` |
| session | `palette` `help` `edit-config` `detach` `quit` `literal-prefix` |

`>` and `<` are the shifted period and comma. A terminal does not report the shift
for punctuation, so `.` and `,` answer to them too — the same deal `/` has with
`?`, and the reason the defaults bind both halves.

`scroll-up` and `scroll-down` have no default key — the wheel does that job —
which is exactly the case the palette exists for. So does `clear-shaders`, which
undoes whatever the program in a pane painted on it
([shaders](shaders.md#prototyping-in-a-pane)); bind it if you leave
`in_band_shaders` on.

`workspaces`, `save-workspace` and `set-purpose` are the other way round: all
three are bound by default, to `w`, `W` and `P`, so the palette is a second way
to reach them rather than the only one.
