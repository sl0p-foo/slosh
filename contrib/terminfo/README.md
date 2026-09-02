# The xterm-ghostty terminfo, for machines that have never met ghostty

Panes advertise `TERM=xterm-ghostty` when the entry resolves, and fall back
to `xterm-256color` when it does not (see `pty_term()` in `src/pty.c`). The
fallback works everywhere; the real entry is better. `slosh
--install-terminfo` writes it into `~/.terminfo`, which is the first place
curses looks, needs no root, and conflicts with no package manager.

The files here:

| file | what |
|---|---|
| `gen.zig` | prints the vendored libghostty-vt's terminfo *source* |
| `xterm-ghostty.ti` | that output, committed for review and provenance |
| `xterm-ghostty.terminfo` | the same, compiled with `tic -x`; what the binary embeds |

The Makefile turns the compiled entry into `build/terminfo.h`, so the binary
carries its own copy: a static tarball on a bare box can fix that box.

## Regenerating, on a re-vendor

The entry is generated **from the vendored tree**, so it describes the
terminal core we actually ship, not whatever ghostty is installed here:

```sh
cd contrib/terminfo
zig build-exe -femit-bin=/tmp/tigen --dep terminfo -Mroot=gen.zig \
    -Mterminfo=../../vendor/libghostty-vt/src/terminfo/main.zig
/tmp/tigen > xterm-ghostty.ti
tic -x -o /tmp/ti xterm-ghostty.ti && cp /tmp/ti/x/xterm-ghostty xterm-ghostty.terminfo
```

Commit both outputs. `tic` warning about "older tic versions" is about the
description field and harmless.
