# Actions

An **action** is a thing slosh can do to a pane, a tab, or the session. split,
zoom, next tab, quit, etc. Every action has a name, and that name is what you bind a
key to ([keys](keys.md)) or type into the command palette (`C-a p`).

Everything below can be bound, and everything below can be run from the palette
without being bound at all. The **key** column is the default binding, if there
is one; an action with no default key is reached from the palette until you give
it one.

## Panes

| action             | key     | does                                           |
| ------------------ | ------- | ---------------------------------------------- |
| `split`            | `Enter` | split whichever way there is more room         |
| `split-cols`       | `\`     | split into columns                             |
| `split-rows`       | `-`     | split into rows                                |
| `close-pane`       | `x`     | close this pane                                |
| `rerun`            | `r`     | run a finished pane's command again            |
| `zoom`             | `z`     | fill the tab with this pane, and back          |
| `minimize`         | `m`     | put it away in the strip along the bottom      |
| `float`            | `f`     | [float](panes.md#floating-a-pane) it, and back |
| `new-float`        | `F`     | a new floating shell, the throwaway terminal   |
| `set-purpose`      | `P`     | tag it with a [purpose](layouts.md#purposes)   |
| `rename-pane`      | —       | rename this pane (double-click does it too)    |
| `clear-shaders`    | —       | clear whatever the program painted on it       |
| `pane-to-next-tab` | `>`     | push it to the tab after                       |
| `pane-to-prev-tab` | `<`     | push it to the tab before                      |
| `pane-to-new-tab`  | `b`     | break it out into a tab of its own             |

## Focus

| action        | key      | does                |
| ------------- | -------- | ------------------- |
| `focus-left`  | `h`, `←` | move focus left     |
| `focus-right` | `l`, `→` | move focus right    |
| `focus-up`    | `k`, `↑` | move focus up       |
| `focus-down`  | `j`, `↓` | move focus down     |
| `focus-next`  | `o`      | the next pane       |
| `finder`      | `s`      | find a pane by name |

## Size

| action          | key            | does                                                     |
| --------------- | -------------- | -------------------------------------------------------- |
| `resize-left`   | `H`, `shift+←` | move the boundary left, or the pane if it is floating    |
| `resize-right`  | `L`, `shift+→` | move the boundary right                                  |
| `resize-up`     | `K`, `shift+↑` | move the boundary up                                     |
| `resize-down`   | `J`, `shift+↓` | move the boundary down                                   |
| `equalize`      | `0`            | give every visible pane an even share                    |
| `float-grow`    | `=` (`+`)      | grow a focused float about its centre                    |
| `float-shrink`  | `-`            | shrink a focused float (through `split-rows` when tiled) |
| `rotate-layout` | `Space`        | turn the layout a quarter turn (four brings it back)     |

## Tabs

| action                          | key         | does            |
| ------------------------------- | ----------- | --------------- |
| `new-tab`                       | `c`         | new tab         |
| `rename-tab`                    | —           | rename this tab |
| `close-tab`                     | `X`         | close this tab  |
| `next-tab`                      | `Tab`       | next tab        |
| `prev-tab`                      | `shift+Tab` | previous tab    |
| `select-tab-1` … `select-tab-9` | `1` … `9`   | go to that tab  |

## Scroll

| action             | key    | does                |
| ------------------ | ------ | ------------------- |
| `scroll-up`        | —      | up a line           |
| `scroll-down`      | —      | down a line         |
| `scroll-page-up`   | `PgUp` | up a page           |
| `scroll-page-down` | `PgDn` | down a page         |
| `scroll-top`       | `Home` | to the oldest line  |
| `scroll-bottom`    | `End`  | back to the present |

## Projects

| action           | key | does                                          |
| ---------------- | --- | --------------------------------------------- |
| `workspaces`     | `w` | go to a project ([workspaces](workspaces.md)) |
| `save-workspace` | `W` | write this tab out as this project's layout   |

## Session

| action           | key          | does                                  |
| ---------------- | ------------ | ------------------------------------- |
| `palette`        | `p`          | the command palette                   |
| `help`           | `?`          | the cheatsheet                        |
| `edit-config`    | `e`          | edit the config, in a pane            |
| `detach`         | `d`          | detach, leave it running              |
| `quit`           | `q`          | quit the session                      |
| `literal-prefix` | leader twice | send the leader itself to the program |

## Notes

`scroll-up` and `scroll-down` have no default key — the mouse wheel does that
job — which is exactly the case the palette exists for. So does `clear-shaders`,
which undoes whatever the program in a pane painted on it
([shaders](shaders.md#prototyping-in-a-pane)); bind it if you leave
`in_band_shaders` on. `rename-pane` and `rename-tab` are unbound too, because a
double-click on the title is the fast path.

`>` and `<` are the shifted period and comma. A terminal does not report the
shift for punctuation, so `.` and `,` answer to them as well: the same deal `/`
has with `?`, and the reason the defaults bind both halves.
