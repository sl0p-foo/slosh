# Keys

Everything is the **leader** (`C-a` by default) and then a key. Pressing the
leader twice sends it to the program in the pane.

`C-a ?` shows a cheatsheet built from the bindings your config actually has. `C-a p` opens the same list as a palette you can type into.

## Defaults

| key                              | does                                                                                                    |
| -------------------------------- | ------------------------------------------------------------------------------------------------------- |
| `Enter`                          | split whichever way there is more room: across the longer side, and the other axis if that will not fit |
| `\` `-`                          | split into columns / rows, when you mean one                                                            |
| `h` `j` `k` `l`, or arrows       | move focus                                                                                              |
| `o`                              | the next pane                                                                                           |
| `H` `J` `K` `L`, or shift+arrows | move the boundary between panes, or the pane itself when it is [floating](panes.md#floating-a-pane)     |
| `0`                              | give every visible pane an even share                                                                   |
| `Space`                          | turn the layout a quarter turn                                                                          |
| `z`                              | (un)zoom this pane                                                                                      |
| `m`                              | minimize pane to bar in the bottom                                                                      |
| `f`                              | [float](panes.md#floating-a-pane) it above the layout, and back                                         |
| `F`                              | a new floating shell, the throwaway terminal                                                            |
| `=` (`+`) `-`                    | grow / shrink a focused float about its centre (`-` splits, when the pane is tiled)                     |
| `x`                              | close this pane                                                                                         |
| `X`                              | close this tab, and everything in it                                                                    |
| `r`                              | run a finished pane's command again                                                                     |
| `P`                              | set this pane's [purpose](layouts.md#purposes)                                                          |
| `>` `<`                          | push this pane to the next / previous tab (a toast says where it went)                                  |
| `b`                              | break this pane out into a tab of its own                                                               |
| `c`                              | new tab                                                                                                 |
| `Tab` `shift+Tab`                | next / previous tab                                                                                     |
| `1`…`9`                          | go to that tab                                                                                          |
| `s`                              | search: find a pane by name                                                                             |
| `w`                              | the projects picker: every project under your roots, open or not ([workspaces](workspaces.md))          |
| `W`                              | write this tab out as this project's layout ([workspaces](workspaces.md))                               |
| `p`                              | the command palette                                                                                     |
| `PgUp` `PgDn` `Home` `End`       | [scrollback](config.md#scrollback) (the wheel works too)                                                |
| `e`                              | edit the config, in a pane                                                                              |
| `?`                              | this list                                                                                               |
| `d`                              | detach, leave it running                                                                                |
| `q`                              | quit the session                                                                                        |
| `C-a`                            | send a literal `C-a`                                                                                    |

**`x`/`X`, `p`/`P`, `w`/`W` and `f`/`F` are deliberately shifted pairs on one
letter.** Close this pane / close this tab; run a command / tag this pane; go
to a project / write this project down; float this pane / float a fresh one.
The shifted half is the same verb on a bigger thing, so the shift is less to
remember than a second letter would have been.

## Rebinding

`C-a` is the default leader key, but if you want it back for start-of-line (and
plenty of people do), it's easy to reconfigure:

```kdl
keys { prefix "ctrl+b" }        // or ctrl+space, or alt+x, or ...
```

A `keys` block **adds to** the defaults rather than replacing them, so binding
one key leaves the rest alone. `"none"` takes a binding away:

```kdl
keys {
    bind "v" "split-cols"       // as well as \
    bind "-" "none"             // and no more splitting into rows
}
```

Modifiers are:

- `ctrl`
- `alt`
- `shift`
- `super`

Named keys are:

- `left`
- `right`
- `up`
- `down`
- `enter`
- `tab`
- `escape`
- `space`
- `backspace`
- `home`
- `end`
- `pageup`
- `pagedown`
- `delete`
- `insert`
- `backslash`
- `minus`
- `slash`
- `comma`
- `period`
- `f1..f12`

the arrows can be written as `←` `→` `↑` `↓` too,
so a chord copied off the cheatsheet is a chord you can paste into a config.

Anything else is the character you press. That includes the shifted ones, which
carry their own shift: `"?"` is `"shift+slash"`, `"H"` is `"shift+h"`, `"|"` is
`"shift+backslash"`.

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

The name in the second half of a `bind` is an **action**. The full list, every action,
its default key, and what it does can be found on the **[actions](actions.md)** page.
Any of them can be bound here, and any of them can be run from the palette without
being bound at all.
