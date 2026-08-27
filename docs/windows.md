# Windows

slosh builds and runs natively on Windows 10/11, on both x86-64 and ARM64. It
is one `slosh.exe` with no runtime dependency beyond the OS.

```bash
make -f Makefile.windows ARCH=x86_64     # cross-compile from mac or Linux
make -f Makefile.windows ARCH=aarch64
```

`ARCH` defaults to `aarch64`. The same command builds natively on the Windows
box itself, with zig on `PATH` and a `make`. The vendored terminal core is
cross-compiled the same way and lands in
`vendor/libghostty-vt/zig-out-win-<arch>`, so the two architectures never share
an output directory.

The binary appears at `build/win-<arch>/slosh.exe`.

## What it does there

Everything the POSIX build does: panes, splits, tabs, detachable sessions, the
config file and its live reload, the control socket, the mouse. Panes run
`cmd.exe` by default — `%ComSpec%` if you have pointed it elsewhere, and
`shell` in the config file wins over both. `slosh ls`, `slosh -s NAME cmd ...`
and `slosh --script` behave as documented.

Sessions live in `%LOCALAPPDATA%\slosh` rather than `$XDG_RUNTIME_DIR`: a
session socket is machine-local by definition and must not follow a roaming
profile onto another host.

## How it works

The port is a compatibility layer, not a fork: `compat/win/` holds stub
`<poll.h>`, `<sys/socket.h>`, `<termios.h>` and friends that redirect to
`include/compat_win.h`, which is force-included ahead of every translation
unit. The existing sources keep their POSIX includes and their POSIX call
sites, and the platform differences are answered in one place.

Three decisions carry most of it.

**Everything the event loop waits on is a socket.** On POSIX a pty master, an
inotify descriptor and a unix socket are all file descriptors, so one `poll()`
covers them. Windows has no such union: a ConPTY hands back pipe `HANDLE`s and
`WSAPoll` waits on `SOCKET`s and nothing else. Rather than rewrite the loop
around `WaitForMultipleObjects` — which caps at 64 objects and cannot express
`POLLOUT` — each non-socket source gets a pump thread that copies it into one
end of a loopback socketpair. `server.c`'s loop is then byte-for-byte the
POSIX one, and only what sits behind the descriptor differs. The config watcher
(`ReadDirectoryChangesW`) and the client's console input reach the loop the
same way.

**Socket descriptors are tagged, not guessed.** A wrapped `SOCKET` is returned
as `SL_SOCK_BASE + slot`, so `read`, `write`, `close` and `poll` can tell a
socket from a CRT file descriptor with certainty rather than inferring it from
the numeric range. That is why `socket()` and `accept()` are shimmed too: an
untagged descriptor escaping into the loop is the one failure mode this scheme
has, and it is worth closing completely.

**A pane is a ConPTY.** `src/pty_win.c` implements the same three functions as
`src/pty.c` — spawn, resize, close — on `CreatePseudoConsole`, with the pty's
`fd` being the pollable end of its pump pair and its `pid` a real Windows
process id, so the existing kill and wait paths work unchanged.

Two details are worth knowing, because both are invisible until they bite.

**A pseudoconsole does not stop the child inheriting your stdio.** A child
attached to one is *supposed* to take its stdio from that console, but
`CreateProcess` copies the parent's standard handles into the child when
`STARTF_USESTDHANDLES` is absent, and those win. With slosh's own stdout
redirected — a pipe under ssh, a file under `--script`, a log under the daemon
— the shell wrote its banner into slosh's output instead of into the pane. The
symptom was a pane with a correct *title* and an empty *body*, because ConPTY
emits the title itself. `pty_win.c` clears the three standard handles for the
duration of the `CreateProcess` call, leaving the child nothing to inherit.

**A ConPTY never reports EOF.** On POSIX the pty master reports end-of-file
once the last slave descriptor closes, which happens as the child dies, and
`pane_pump()`'s `read()` returning 0 is the entire mechanism by which a pane
learns its program is gone. A ConPTY gives no such signal: the output pipe
belongs to the *pseudoconsole*, not to the child, so it stays open after the
child exits and the read never ends. A pane whose shell had been told `exit`
sat there looking alive forever. So the exit is waited for explicitly, on the
process handle, and `ClosePseudoConsole` is what then produces the EOF — the
console flushes what the child left behind and closes its end, and the output
pump sees the end of the stream exactly as it would on a pty.

**There are two quoting conventions, and cmd.exe uses the other one.** Windows
passes a command *string*, not a vector, so the quoting rules belong to whoever
parses it at the far end — and there are two sets. Ordinary programs are parsed
by `CommandLineToArgvW`, where a literal quote is written `\"`. `cmd.exe` is
not: it has its own rules, in which `\"` is a backslash followed by the end of
a quoted section. Applying the first set to the second turned
`cmd /c notepad "C:\...\config.kdl"` into an editor being handed
`\"C:\...\config.kdl\"`, backslashes and all. So `cmd /c` is given the rest of
the line verbatim — the caller composed a complete command with its own
quoting, exactly as it composes one for `sh -c`, and cmd.exe is the thing that
should parse it. Everything else is still quoted for `CommandLineToArgvW`.

**Paths are mixed, and only some programs mind.** slosh builds paths with `/`
throughout, because that is what it splits and compares on. The environment
supplies `\`, so the moment a literal is joined to a variable the result is
mixed: `$HOME` is `C:\Users\you`, and the config lands at
`C:\Users\you/.config/slosh/config.kdl`. The CRT opens that without complaint,
which is why `--check` reported it as fine — but a GUI file dialog rejects it
outright, and `C-a e` produced a notepad saying "Not a valid file name" about a
path that unambiguously existed. Anything handed to another program now goes
through `sl_path_native()` first, which canonicalises it and settles the
separators; the internal representation is left alone.

The reference counting is also why the per-pty state is not simply freed by
`pty_close`: two pumps, a exit watcher and the caller all reach
it, and a pump blocked on a dead pipe must not be waited for — that would hang
the session on the one program that misbehaves. Whoever finishes last frees.

## What is different

- **No `SIGWINCH`.** A resize is noticed by comparing the console size on a
  short poll timeout, which is why the client's `poll()` there is bounded
  rather than infinite.
- **No `fork`.** The session daemon is started as a fresh detached process that
  reconstructs its arguments on the command line (`--server`), rather than as a
  copy of the process that asked for it.
- **No process groups on a pty**, so "what is this pane running" is answered
  with the most recently started descendant of the pane's shell — the process
  `tcgetpgrp()` would have named. Windows offers the image name rather than a
  full argv without reading another process's memory, so pane titles are
  correspondingly shorter.
- **No editor is guaranteed.** `C-a e` prefers whichever console editor is
  actually installed — `nvim`, `vim`, `vi`, `nano`, `micro`, `hx` — because the
  obvious fallback is the wrong shape: `notepad` is a GUI program, so a pane
  running it draws nothing and looks broken. If none is found it still opens
  notepad, and says so, rather than leaving you an empty pane to puzzle over.
  `EDITOR` and the config's `editor` are honoured ahead of the search.
- **`--check`, `--dump-config`, layouts, workspaces and shaders** are
  unaffected: they never touched a pty or a socket.
