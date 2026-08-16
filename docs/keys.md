# Keys

Everything is the **leader** (`C-a` by default) and then a key. Pressing the
leader twice sends it to the program in the pane.

`C-a ?` shows a cheatsheet built from the bindings your config actually has, so
it cannot disagree with your keyboard. `C-a p` opens the same list as a palette
you can type into.

## Defaults

| key | does |
|---|---|
| `\` `-` | split into columns / rows |
| `h` `j` `k` `l`, or arrows | move focus |
| `o` | the next pane |
| `H` `J` `K` `L`, or shift+arrows | move the boundary between panes |
| `=` | give every visible pane an even share |
| `Space` | turn the layout a quarter turn (four brings it back) |
| `z` | zoom this pane to fill the tab, and back |
| `m` | minimise it into the strip along the bottom |
| `x` | close this pane |
| `r` | run a finished pane's command again |
| `>` `<` | push this pane to the next / previous tab (a toast says where it went) |
| `b` | break this pane out into a tab of its own |
| `c` | new tab |
| `Tab` `shift+Tab` | next / previous tab |
| `1`…`9` | go to that tab |
| `f` | find a pane by name |
| `p` | the command palette |
| `PgUp` `PgDn` `Home` `End` | scrollback (the wheel works too) |
| `e` | edit the config, in a pane |
| `?` | this list |
| `d` | detach, leave it running |
| `q` | quit the session |
| `C-a` | send a literal `C-a` |

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

`sl0ppty --check` reads a config and says what it could not honour, one problem
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
| panes | `split-cols` `split-rows` `close-pane` `rerun` `zoom` `minimize` `rotate-layout` `clear-shaders` `pane-to-next-tab` `pane-to-prev-tab` `pane-to-new-tab` |
| focus | `focus-left` `focus-right` `focus-up` `focus-down` `focus-next` `finder` |
| size | `resize-left` `resize-right` `resize-up` `resize-down` `equalize` |
| tabs | `new-tab` `next-tab` `prev-tab` `select-tab-1` … `select-tab-9` |
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
