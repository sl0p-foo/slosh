# sl0ppty — design

An opinionated terminal multiplexer in C, built on
[libghostty-vt](https://github.com/ghostty-org/ghostty).

Status: **agreed contract, M0 in progress.** This file is what we build against.
It supersedes the riff in `SEED.md`.

## Why

`sl0ppi` is a fork of zellij carrying 62 patches. Reading them, almost none of
the work is *multiplexing* — it is fighting a codebase whose assumptions differ
from ours, at ~5s per iteration on a good day and 5m16s for a release build.
The valuable part is the **opinion**, and the opinion is small enough to
implement directly against a VT library that already does the hard half.

libghostty-vt is that library. What it gives us, from its C headers:

- the full VT/ANSI state machine, scrollback, page compression
- two-layer dirty tracking (global + per-row) — precisely our render loop
- resolved per-cell fg/bg, styles, UTF-8 grapheme clusters
- selection incl. semantic word/line/output selection
- kitty graphics with placement tracking, OSC 8 hyperlinks per cell
- key and mouse **encoders** (event → the bytes an app expects, honouring that
  pane's own mode state)
- an effects callback table: `WRITE_PTY`, `TITLE_CHANGED`, `PWD_CHANGED`,
  `CLIPBOARD_WRITE`, `BELL`, `DESKTOP_NOTIFICATION`, `PROGRESS_REPORT`, `MODE`,
  and **`UNKNOWN_SEQUENCE`**

**Correction, found by reading the header rather than the summary:**
`UNKNOWN_SEQUENCE` today reports **APC sequences only**
(`GHOSTTY_TERMINAL_UNKNOWN_SEQUENCE_APC` is the sole tag), not unknown OSC. So
it is *not* a free OSC 5577 hook. Three consequences, all cheap:

1. **OSC 5577 (D1) arrives via a side-channel scanner** on the pty byte stream:
   ~100 LOC that spots `ESC ] 5577 ; … ST` and lifts it out. The bytes still go
   to lib-vt, which discards an unknown OSC harmlessly, so nothing is drawn.
   The hook point is already marked in `pane_pump()`.
2. **Protocol v2 should be APC**, not OSC — APC *is* delivered as a first-class
   effect, and no terminal will ever render it. The v1 OSC form stays forever
   for the existing pi extensions.
3. Worth an upstream issue: a generic "unhandled OSC" callback. Mitchell's
   library, our use case, small patch.

### lib-vt assumes nothing a host terminal would

It is a VT *library*, not a terminal, so host policy is ours to set. Found by
the cursor never appearing:

- **DECTCEM (mode 25) starts off.** The cursor is invisible until the host says
  otherwise. We set it via `MODE_DEFAULT`, which updates the current value *and*
  the one restored by RIS, so a program that resets the terminal does not lose
  its cursor.
- **Grapheme clustering (mode 2027) starts off**, so flags and ZWJ emoji occupy
  several cells. We default it on, matching the outer terminal we assume (D11).

Both are set in `pane_new()`. Expect more of these; the rule is that anything a
terminal emulator would decide is not decided for us.

### The option-setting convention that will bite you

`ghostty_terminal_set()` takes `const void *value`, and what that points at
depends on the option's type:

| header says | pass |
|---|---|
| `Input type: GhosttyTerminalWritePtyFn` | the function pointer **itself** |
| `Input type: void*` (userdata) | the pointer **itself** |
| `Input/output type: GhosttyTerminalModeConfig *` | a pointer to the struct |

A pointer-shaped value is passed as itself. Passing `&fn` compiles, links, and
stores the address of a stack local as the callback — which survives exactly
until `pane_new()` returns, and then jumps into a dead frame. It went unnoticed
until the first program asked the terminal a question (OSC 2, DSR); both paths
now have regression tests.

### What we own

1. **Input decoding.** lib-vt encodes events *out* to a pane; nothing decodes
   what the outer terminal sends *in*. We write that parser: legacy keys, kitty
   CSI-u, SGR mouse, bracketed paste, focus events. Because we decode to a
   semantic event and re-encode per pane against *that pane's* modes, kitty
   keyboard passthrough works properly — which tmux and zellij both fumble.
   **Done** (`src/input.c`, ~500 LOC): 40 table-driven cases plus a live check
   that drives one client into two panes with different negotiated modes and
   asserts each receives the encoding *it* asked for.
2. **Compositor + diff → ANSI.** Turning N panes of cells into a minimal byte
   stream for the real terminal.
3. **pty spawn, event loop, layout tree, sockets, config.**
4. **Kitty graphics re-emission** (id remapping across panes and scroll).
   **Done.** lib-vt tracks the images and placements; we walk them each frame,
   transmit an image once (`a=t`), place it every frame (`a=p,C=1`) after the
   cell diff, and delete placements that vanish. Ids are remapped into a high
   range so two panes using image `7` stay two images, and so nothing collides
   with images the client's terminal already had.

   The cropping rule is the part worth remembering: **a placement is N pixels
   drawn across M cells, so the placement itself tells you the pixels per
   cell** — no need to know the client's font metrics. Clipping at a pane edge
   or at the top of the viewport moves the *source rectangle*; reducing `c`/`r`
   alone scales the image into fewer cells instead of cropping it.

Budget: **under 10k LOC**, zero deps beyond libc and lib-vt, static musl
binary, sub-second full rebuild.

## Two structural commitments

These exist because of two lessons the sl0ppi fork paid for in days.

### One geometry

The `+` split button shipped twice while being invisible on screen, rendering
correctly in 265 passing unit snapshots, for four independent reasons — three of
them "the code that paints and the code that hit-tests derived the rect
separately, and disagreed."

> **Every painted interactive element emits its `(rect, action)` into a hit-list
> as it is painted, by the same code, in the same pass. A click is a lookup in
> that list.**

Drawing and hit-testing cannot disagree, because there is only one of them.

### Headless from day one

From the sl0ppi roadmap, four separate times: *unit tests were necessary and
insufficient.* Every real bug was found by reading the screen.

> **`sl0ppty --script` runs the entire compositor with no tty: commands in
> (`send`, `raw`, `settle`, `resize`, `snapshot`), and the composited screen out
> as JSON — rows of text, style runs, cursor, hit-list.**

Tests are "drive these events, assert this screen". `make test` is **2.4s for
59 checks**; `make test-live` keeps the handful that genuinely need a tty (raw
mode, the diff emitter, SIGWINCH, the prefix key). The command vocabulary is
deliberately the one M2's control socket will speak, so the harness moves over
unchanged.

Styles are emitted as **runs**, not per-cell objects: a test wants to say
"columns 0..5 of row 0 are bold red", and 1920 cell objects per snapshot is
unreadable.

## Decisions

| # | decision |
|---|---|
| **D1** | **OSC 5577 stays byte-compatible** with the sl0ppi fork, so the five existing pi extensions work unmodified. Delivered by a side-channel scanner on the pty stream (see the correction above), with a versioned `hello` handshake added on top — the fork's protocol is unversioned, which is a known risk — and an APC-based v2 once anything new is written against it. |
| **D2** | **Config is a hand-rolled KDL subset** (`kdl.c`, 300 lines, no dependency): nodes with arguments, `key=value` properties, children, `//` and `/* */` comments, `;` terminators for single-line nodes. `config/config.kdl` documents every setting by *being* the defaults. A broken file costs a warning on stderr and nothing else; `{"cmd":"reload"}` re-reads it live and refuses a file it cannot parse rather than half-applying it. **Two schemas share the syntax** -- a config and a layout -- so layout files are named `*.layout.kdl`: the extension says how to read the file and the stem says what is in it, the way `docker-compose.yml` does. Naming them something other than KDL would trade one wrong signal for another, since one parser really does read both. A name cannot enforce anything, so each loader also *says* which document it was handed (`this is a layout, not a config`, and the reverse), both answers measured against the same list of settings the loader reads. That list is what makes an unknown top-level node reportable at all: a loader that asks a document only for the keys it knows never notices what it was given, which is why a mistyped setting used to do nothing quietly and a whole layout linted clean. |
| **D3** | **Clean JSON control API** (one JSON object per line on a unix socket): `{"cmd":"new-tab","purpose":"project:x.deadbeef"}` -> `{"ok":true,"id":2}`. We do *not* mimic `zellij action`'s surface. A bare-verb form (`panes`, `snapshot text`) is kept as a human/harness alias and runs the same code, so a script cannot drift from what the API does. The `sl0ppi` CLI is ported on top later; `up`'s idempotency and D9's fail-open property carry over. |
| **D4** | **Scrollback yes** (free from lib-vt), **copy-mode UI later.** "Free" was true of the *machinery* and quietly false of the *size*: handed no limit, lib-vt picks its own — 10,000 **bytes**, which measured at 622 lines of an 80-column pane, less than one `make` run. A promise to keep history that keeps a screenful of it is the kind of thing a library default gets to decide only once. So `scrollback` is lines, defaulting to the 10,000 every other multiplexer settled on, and `scrollback_bytes` is the ceiling that count runs into: lib-vt applies whichever limit is reached first, and it has to, because a line count cannot see how wide the terminal is or how many styles a program used and a byte count cannot see how many lines that bought. Measured at 10,000 lines of styled output: ~6 MB at 80 columns (the ceiling never bites), ~15 MB at 200 (it costs 2% of the lines), ~32 MB at 400 (it costs half). Invisible at ordinary widths and a brake at extraordinary ones, which is the job. There is deliberately **no unlimited**: the ceiling is per pane, sixty panes is sixty times it, and "unlimited" across that many is a memory leak with a friendly name — a negative is refused rather than wrapped into one. Both are read when a pane is created, so they apply to the next pane you open rather than to panes already holding output somebody is reading, which is the same rule and the same reason as `shell`. |
| **D5** | Binary, session dir, socket and config are all named **`sl0ppty`**. |
| **D6** | **Responsive layout is a pure function** — see below. If *any* node cannot give its children their floor, the **whole tab** flattens — asked of the tab, answered once, because half a screen of panes beside half a screen of headers explains neither what happened nor what to do about it. The check is the real layout run with resizing switched off, not a second implementation of the same arithmetic. Flattening means: every pane under it becomes one row in a flat list with the focused one opened below them, rather than a nest of headers. Nesting is exactly what there is no room to express, and in that state a reader wants *which pane*, not *which arrangement*. It also makes every pane reachable — collapsing a subtree to a single header gave every leaf inside it the same rect and one hit for the first of them, so the rest could not be clicked at all. |
| **D7** | **Server renders, client is dumb.** The client forwards raw bytes and paints what it is sent — it does not even decode, so the decoder can change without redeploying clients, and a bug there costs a frame rather than a session. The same socket is the scripting API. Detach/reattach is table stakes because agents keep running. |
| **D8** | **Panes and tabs carry `purpose=`**, sl0ppi's semantics kept verbatim, including the trust model: *declared purposes outrank in-band ones and cannot be overridden*, so `cat hostile.txt` cannot relabel a project tab. Declared means "from a layout or an operator", i.e. the control path; the in-band path (OSC, M4) always passes `declared:false` and is refused against a locked slot. Sanitised on ingest to `[A-Za-z0-9_.:/-]`. |
| **D9** | **No wasm plugins.** Status bar and pane finder are built in. A "plugin" is a subprocess speaking the control protocol — with exactly one exception, shaders, for the reason in D15. |
| **D10** | **One attached client** in the MVP. The protocol allows N (read-only observers are what sl0p.foo actually wants); the multi-user *rendering* path is not built, because it is where the fork's phantom-client and `MY FOCUS AND:` bugs live. |
| **D11** | **No terminfo/ncurses.** We are opinionated about the outer terminal: emit a known-good modern subset, probe with DA/XTGETTCAP where we must. |
| **D12** | **Vendor lib-vt source**, pinned by commit in `vendor/libghostty-vt.vendor.json`, with a patches file if we ever need one. We needed one: `vendor/patches/0001` stops a screen clear from freeing transmitted kitty images, which broke every program that uploads once and places per frame (see D17). Patches are applied to the committed tree and listed in the vendor json, so a build has them and a re-vendor knows what to re-apply. zig is a build-time dependency only, and doubles as our C compiler and static/musl cross-linker. |
| **D13** | **Shaders are a colour pass over cells, and *which* cells is the pass's, not the shader's.** A shader is a pure function from (cell, position, parameters) to that cell's colours, run between `pane_compose()` and the split guide — so a content pass covers the contents and nothing else by paint order, not by a rule anyone has to remember. Frames are shaded by a pass of their own over a rect of their own (D19); no shader knows the difference, which is the point. Cells only: fg, bg and attrs may change, text and width may not, because rewriting text would desync selection and copy (which read the terminal, not the screen). Each cell is shaded independently, so positional effects are expressible and convolutions are not; that is the price of needing no second buffer. Shaders chain in order, each a complete pass. Attached per pane by internal policy only — not over the control API and not in-band, so a program cannot restyle itself. A cell whose colour is "terminal default" is materialised to a **configured** `default_fg`/`default_bg` first: we never learn the client's real default, and most text is default-coloured, so the alternative is a dimmer that visibly does nothing. Panes with no shader skip the pass entirely and still defer to the terminal. Which shader a pane gets from the session is a **table of states** — dragging, drop_hover, drop_target, dead, suspended, scrolled, unfocused — each naming a chain. A pane is usually in several at once, so exactly one wins: the first to match, in a fixed order running from the most deliberate to the most ambient. **Which states ship a default is a line, not a mood**: a pane that is *not live* gets one — dead, suspended, scrolled, where the cells are showing something other than a running program's present and nothing else would tell you. `unfocused` is a taste rather than a fact and gets one anyway, through a knob of its own (`dim_unfocused`) rather than through the table: the objection to shipping a taste was never the taste, it was that turning it off should not require knowing the states block exists, and a single number fixes that. Writing the chain by hand still wins over the knob, including writing an empty one. `dragging` keeps no default, because the pane in your hand keeping its colour is what makes it lift off the page. The corollary is that a collapsed header, which the pass never reaches because a header is chrome, has to carry those same three facts in words. Fixed rather than configured, because it is a ranking by urgency and not a preference. They do not stack, because two reasons to be grey compound into one muddy grey that reads as neither. `dead` is one of them because a pane whose program exits is now kept until it is dismissed (D14); it was excluded while there was no frame it could ever draw. All of it is **derived every frame, never attached and remembered**: focus and drags move through too many paths to keep an attachment in sync with, and a pane left grey by the one path that forgot to clear it is a bug that only appears in front of someone. Same rule as the rect and the collapsed flag. |
| **D14** | **A pane that was given a command outlives it.** When such a program exits, the pane stays: it keeps everything it printed, writes one line into its own backlog saying how it ended (`[process exited: status 3]`, `[process exited: signal 9]`), says the same thing in its frame and in the status line, and offers the only two verbs still meaningful — `[re-run]` and `[close]`. Re-running spawns the same argv in the same cwd, in the *same pane*: same id, same place in the tree, same terminal, so the run that ended stays above the new one in the scrollback and nothing that referred to that pane has to be told anything. The old behaviour reaped the pane before the next paint, which meant the last thing a failing command printed vanished together with the status that would have explained it, and a mistyped command in a fresh session closed the session. Which panes this applies to is the *command*, not the provenance: a pane told to run something is part of the session's shape and its death is news, while a shell you split off is finished when you type `exit` and a corpse to dismiss there is fussiness. A shell is a shell whether a layout made it or a keystroke did, which also means a session restored from `dump-layout` behaves like the one it was dumped from. `keep_dead` moves the line: `all` keeps shells too, `none` keeps nothing. Two consequences are load-bearing rather than incidental: the pty fd is **closed** the moment EOF is seen, because an EOF fd left in the poll set is readable forever and would spin a session at 100% CPU behind a pane that looks idle; and the exit status is collected with a **bounded** wait, never a blocking one, because a program that closes its terminal and keeps running must not be able to hang the session — losing that race costs the words "status 0" and nothing else. |
| **D15** | **Shaders may be loaded as native shared libraries**, from a directory beside the config. This is the one hole in D9 and it is argued for rather than drifted into (and D16 has since removed most of the *need* for it). A shader is called per cell, per pass, per frame — tens of thousands of times at up to 120Hz — so the subprocess model D9 prescribes cannot reach that path at all, and the measured alternative, interpreting a bytecode per cell, is 56ns against 8ns for the compiled function. What is loaded is also the narrowest possible thing: a pure function from one cell and its position to that cell's colours, unable to touch text, width, neighbours, panes or the session, because `shader_abi.h` is the only header it sees and nothing else is declared in it. On trust: the config file can already say `shell` and a layout can already say `command=`, so a file in the config directory can already run anything as you — a `.so` in the same directory is not a new boundary. It is refused unless it declares our ABI version *and* matching struct sizes, so a stale plugin fails at load instead of garbling colours. Nothing is ever unloaded: a `shade_fn` may be sitting in a live config or on a pane, so `dlclose()` would be a jump into an unmapped page — a reload therefore picks up newly *added* plugins only, and replacing one needs a new session. |
| **D16** | **A shader's `amount` may be an expression**, compiled to bytecode at config load. The scope is the argument: every built-in here is already `colour_op(amount)` with the amount a pure function of position, so the part worth handing to a config is the *number*, not the colour maths. That keeps mixing in C, gives an expression no way to produce an invalid cell (it produces one byte), and makes the result cacheable — a program that does not read the clock is evaluated once per cell into a map and reused until something it reads changes, so a config-written positional shader measures 5.2ns/cell against a compiled vignette's 6.1ns. Reading the clock costs 35ns/cell, per frame, which is the honest price of animation and is opt-in. The language is **total**: no loops, no recursion, no jumps — even `?:` evaluates both sides and selects, because nothing in it has an effect. So evaluation is O(program length), a config cannot spin, and there is nothing to sandbox; division by zero is 0 rather than a trap, because an expression that is wrong should look wrong rather than take the session with it. What a program reads is **derived by the compiler**, not declared, and that dependency set *is* the cache key — a stale map is unrepresentable rather than merely unlikely, which is the same rule the layout follows. An expression that does not compile drops that one shader with a warning: not the config, and not a half-strength version of what was asked for. |
| **D17** | **Images need pixels, and we were not carrying any.** Kitty graphics worked in the test suite and not in real life, because three separate things all quietly assumed a cell has no size. `pty_spawn`/`pty_resize` left `ws_xpixel`/`ws_ypixel` zero, so a program asking the tty how big a cell is got nothing and guessed. `ghostty_terminal_resize` was called with `0, 0` for the cell size, so lib-vt could not work out how many cells an image covers when the placement did not say — and a placement that covers zero cells is dropped, which is a picture that silently does not appear. And the vendored lib freed image *data* on `ED 2`, so a full-screen program lost everything it had uploaded. The fix runs the whole way through: the client reads its terminal's pixel size (`TIOCGWINSZ`, re-read on every resize because a window can move to another monitor), `MSG_HELLO`/`MSG_RESIZE` carry it, the app hands it to every pane in every tab, and each pane gives it to both its pty and its terminal. Where nobody knows, the default is **8x16 rather than 0**: a plausible cell means an image appears at roughly the right size, and zero means it does not appear at all. Tested end to end, including that a program can read real pixel dimensions out of its own pty. **Re-emission has to preserve what the program meant, not what it currently covers.** Two things fall out of that. A placement carries a sub-cell offset (`X=`/`Y=`, where the image starts *inside* its first cell); dropping it is invisible in a still picture and makes anything moving jump a whole cell at a time. And `c=`/`r=` mean *scale into this many cells*, so they are passed on only when the program asked for them — sending the count a natural-size image happens to cover makes it change size as it slides across a cell boundary, because that count goes up by one. Cropping follows the same split: a natural image is cropped by the screen pixels the pane has left (measured from the offset, not the cell edge), a scaled one proportionally, because a lost cell is worth `source/cells` pixels there. **And a program may prefer to *ask*.** `TIOCGWINSZ`'s pixel fields are the first way to find the cell size and XTWINOPS (`CSI 14/16/18 t`) is the second, which is what a program reaches for when the pty fields come back zeroed — as they do on the far side of an ssh hop. We answered none of it, so the query timed out and the program fell back to a guess; 10x20 against a real 9x22 cell is every image stretched by a fifth, and it looks like a bug in the program. `GHOSTTY_TERMINAL_OPT_SIZE` answers all three from the pane's own geometry (lib-vt encodes the reply). Mode 2048, the in-band report on resize, already worked: it is answered from the size handed to `ghostty_terminal_resize`. |
| **D18** | **We decode PNG ourselves, with the stb_image we already vendor.** lib-vt ships no image codec: built as a library its `sys.decode_png` hook is null, and a null hook rejects every `f=100` transmission outright — no image stored, no placement, nothing to re-emit. A program that uploads a PNG (which is what a program with a picture on disk does) drew nothing at all, silently, while raw RGB worked. Found because ida-tui's startup logo never appeared. PNG is the only format this can ever be about: the protocol has exactly three (`f=24`, `f=32`, `f=100`) and the first two are raw pixels lib-vt already handles, and `decode_png` is the only codec hook it has — so there is no scope to creep. Decoding only, never encoding; the client is handed RGBA. The decoder is `vendor/libghostty-vt/src/stb/stb_image.h`, which is **already in the tree and already pinned** by the vendor commit — upstream compiles that same header with the same `STBI_ONLY_PNG` for the same reason (`src/stb/stb.c` says so), it is simply not part of the `-Demit-lib-vt` build. So this is not a new dependency, it is a file we already ship finally being compiled, and `src/png.c` is a hook and a copy around it. Hand-rolling a subset decoder was considered and rejected: the bulk of a PNG decoder is a DEFLATE inflate, and "just the colour type ida-tui uses" breaks on the first palette PNG, which is what every png optimiser emits. That is a lot of new bounds-checked parsing, fed by whatever a pane decides to send, to replace a file already in the repo. Two consequences to know: we re-emit **decoded** pixels, so a 819KB logo goes to the client as 3.3MB of base64 (once per image, again after a reattach clears `sent`) — passing the original PNG through would be cheaper but lib-vt hands back only the decoded form; and lib-vt caps image storage at **10MB** in library builds, so images are bigger than they look. |
| **D19** | **The finder and the command palette are one picker with a subject.** They are the same object — a modal, a query, a filtered list, one selection, the same keys and the same mouse rules — differing only in what fills the rows and what Enter does with one. Building the second as its own machine would have meant two of everything, and the pair would have drifted the way the pane title editor and a tab label editor would have if they had not been made one thing with a subject. So there is one `picker` state (`PICK_FINDER`/`PICK_PALETTE`), one draw, one key handler, and a `pick_row_t` of three columns that both fill: a dim left (a tab, a group), the thing itself, and a dim right (a purpose, the chord). Choosing goes through the row's own action string, so the keyboard and the mouse cannot take different paths to the same row. **The palette lists actions that are not bound to anything**, which the cheatsheet by design cannot — it is built from the binds, so an action with no key does not appear, and that is exactly the action you have no other way to reach. Where a binding does exist the palette shows it, so using the palette teaches the key that would have skipped it; where several exist it shows them all, because a config's binds add to the defaults rather than replacing them and naming only the first would hide the one the user chose. Two actions are left out: `literal-prefix`, which means "type this key" rather than "do this thing" and would send the prefix to a pane hidden behind the list, and `select-tab-N`, which is positional — a row saying "go to that tab" cannot say which. Running is by `action_t`, not by synthesising the keystroke that would have caused it: a palette that had to invent input to do its job would be lying to the rest of the program about what happened. |
| **D20** | **Chrome is a rect too, so it is a pass too.** `where="chrome"` on any entry in `shaders` or in a `states` block runs that pass over a pane's **frame** — border, padding, title, buttons — instead of its contents. Nothing else changes: same registry, same `amount`, same expressions (D16), same chain order, one new word. It is one pass over the frame's *whole* rect with the contents punched out as a hole, not four passes over four sides: an effect has to be able to travel round a border, and four passes would each count `x`/`y` from zero and restart at every corner. The hole is what keeps the two honest — a skipped cell is never visited, so a full-strength chrome tint cannot reach your text and a full-strength content tint cannot reach the border. The chains are stored separately rather than as a flag on each shader, because a pass *is* a chain and a rect, and because `shader_abi.h` has no business knowing where a session chose to run an effect. This is also the only kind of pass that can colour a pane a small window has collapsed to a single row (D6), which is the state where a colour is worth the most. **Animation needs a clock**, so `app_next_deadline_ms` reports one while a chain that reads `t` ran over the last frame — derived from what was applied, like everything else about a frame, so a pulse hung off `dead` costs `anim_ms` while a pane is dead and nothing for the rest of the session. Without it a border froze the moment you stopped typing, which is the bug that makes an animated shader look broken rather than absent. A **`bell` pane state** and a **`since`** variable came with it, because "flash the border when a pane rings" needed both and neither existed: a bell had no state to hang a chain off, and `t` says what time it is rather than how long ago something happened, so a one-shot effect was not expressible at all. `since` is milliseconds since the pane entered its current state, stamped in the one function that decides the state — a transition is the one thing a frame cannot tell you, so it is remembered, but it is remembered where it cannot disagree with the state it dates. `bell` ranks above the ambient states and below `dead`: a pane that rang while you were elsewhere is news, and a pane that rang and then exited is a pane that exited. It ends when you look at the pane, because that is what answering a bell is (the frame clock stops with it). Shipped off by default — `bell_indicator`'s quiet mark stays the default, and an animated default would spend a frame clock on every session. **`channel="fg"|"bg"|"both"`** came from the first border flash actually being looked at: `tint` mixes both of a cell's colours, and a frame's background is the terminal's own default, so the pass materialised it to our *guess* at that default and mixed towards the colour — a dark rectangle behind the glyphs, left there until the pane changed state. The mask is enforced by the pass, not by the shader: the cell's other colour is snapshotted before materialising and put back after, so it works for a built-in and a plugin alike, an unset colour goes back to *unset* rather than to our guess, and no shader grows an fg-only variant of itself. It is one byte tested once per pass, not per cell, and 0 means both so every shader built before it behaves as it did. |
| **D21** | **A project is a directory; a workspace is the tab it occupies.** Two words because they are two things, and conflating them is how a session grows a second source of truth about what it is looking at. The directory is found rather than named: a `project_roots "~/dev" "~/work" depth=2` is scanned for subdirectories holding a `sl0ppty.layout.kdl` — or a `.git`, which is the project that has not said what it needs yet, and without which `~/dev` with forty checkouts and three layout files is a picker with three rows. A directory that *is* a project is never descended into, so the walk stays off `node_modules` without anyone writing a rule about `node_modules`. **Identity is the tab's `purpose`**, `project:<name>.<hash8>` of the resolved path — the shape D3 already published — so there is no second field to disagree with it, membership is a string comparison, and a dumped session restores it for free because a dump already writes tab purposes and applying one already locks them (D8). Hashed on the *path* rather than the name, so two worktrees of one repo are two workspaces, which is what they are. A tab whose layout gave it a purpose of its own keeps it and is simply not a member: overwriting a declared purpose is the one thing D8 forbids, and the reply counts the ones it left alone rather than letting that be a surprise later. **Nothing watches the filesystem.** The list is derived when it is asked for — one `readdir` per root and two `stat`s per entry, forty checkouts for eighty-one syscalls next to the keystroke that asked — and an answer nobody remembered cannot be stale, which is a promise no watcher can make on a bind mount. The picker scans once as it opens, not per frame, because `draw_picker` asks for its rows every frame and a `readdir` per repaint is a filesystem walk at 120Hz. The one thing a scan cannot see is a project's layout file changing under a workspace that is already open, and that is deliberately not answered by re-applying it over running processes: `workspaces` reports the file's `mtime`, and drift is derivable outside without this session storing a byte. **Opening is idempotent** (`sl0ppi up`'s property, D3): the same request twice is one workspace, focused, and the answer says which branch it took so a caller never has to ask first. **Saving is the inverse, and it is the whole of onboarding.** `save-workspace` is `dump-layout` for one tab, relative to the project, written beside it — the same dumper, so there is one answer to "what does this tab look like written down". Four things made that possible and none of them were true before: a relative `cwd=` in a layout file now resolves against that file's directory (the rule `include` already followed, applied to the other half of the same syntax), a dump can be asked for one tab against a base, a pane from a split now starts where the pane it came out of stands rather than where the server was launched — which had been quietly writing an absolute path to somebody else's directory into every saved project — and **a dump now records what a pane is actually running**. That last one is what turns arranging a project and writing it down into one act instead of two: `label` only ever knew what a *layout* said, so a pane you split and typed `npm run dev` into had nothing to say for itself and came back as a bare shell. The kernel knows what the label does not — a pty has a foreground process group, and that group is by definition the job that owns the terminal — so the answer is read rather than guessed, the way `live_cwd` already reads `/proc/PID/cwd` rather than trusting where a pane was started. Three things are deliberately not commands: a shell at a prompt (its group is the pane's own, and `command="zsh"` on every idle pane restores a session where each shell runs inside a shell), a background job (it does not own the terminal, and resurrecting `&` jobs in the foreground describes a session nobody had), and an ephemeral pane (restoring one reopens a file somebody finished with). A declared label still outranks a live process, because it survives the program exiting (D14) and re-saving must not degrade `npm run dev` into whatever the process tree looks like this minute. argv is joined with shell quoting, since `command=` is run through `/bin/sh -c` and an argument with a space in it would otherwise come back as two. A saved project defaults to `suspend="commands"`: the pane running this morning's dev server is written asleep, because a *project's* layout is a statement of shape and a *session's* dump is a statement of fact, and starting a dev server on every open is exactly what `suspended` exists to prevent — which is worth much more now that there are commands to suspend. What is **not** built: no guessing commands from `package.json` or `Cargo.toml` (a table of guesses that is wrong on day one and stale by year two — recording what you actually ran makes it unnecessary), no watcher, and no auto-save on close, because a layout file is a commit-sized decision. |

### D6 — responsive layout as a pure function

A grid of panes squeezed onto a small screen is unusable, so we need this. But
zellij does it with swap layouts + a remembered position + synthesized stack
layouts, and it produced the roadmap's "panes stacked after a collapse/expand
cycle" bug (a feasibility check counted `visible_panes_count()`, which a stack
lies about).

Instead:

```
layout(tree, rect) -> (rects, collapsed_set)     recomputed every frame
```

Each split node carries a min-size policy. When its rect cannot satisfy its
children, **that node** flips mode locally:

- `split` — children side by side (normal)
- `stack` — children collapse to title rows, one expanded
- `rail`  — one child takes the bulk, the rest become a monitored strip

No `.swap.kdl`, no remembered state, no synthesized layouts. The bug class is
unrepresentable because there is no state to go stale, and `rail` is the thing
the fork kept almost-reinventing: with agents you want *one big pane you are
reading* plus *a strip of small ones you are watching*.

**As built (M5).** `split` and `stack` are in; `rail` is not needed yet,
because a stack of headers *is* the monitored strip:

```
   1  2  +                                       5 panes
   pi · ready───────────────────────────────────────────
   pi · ready───────────────────────────────────────────
   pi · ready───────────────────────────────────────────
  ╭────────────────────── pi ────────────────────────+─╮
  │                                                    │
  ╰ ready ──────────────────────────────────[continue]─╯
```

A header row carries the pane's title *and its OSC 5577 status*, so a
collapsed agent is still legible. Three properties that fall out of the design
rather than being coded:

- the expanded child is simply the one holding focus, so **focusing a
  collapsed pane expands it** — including from the finder, and by clicking its
  header;
- **a hidden pane is never resized**, so its program does not reflow while you
  are not looking at it;
- a narrow/wide cycle returns the *exact* rects it started with, and a pane
  added afterwards is not stacked. That is the fork's bug, asserted directly.

When not even one header row per sibling fits, the node degrades to showing
only the focused subtree. A pane one cell tall helps nobody, and rects that do
not fit on the screen help less.

### Connections are not clients

The server holds a set of connections. A connection becomes *the* display client
only by sending `MSG_HELLO`; anything else — a control command, `ls` probing
whether a socket answers — is just a connection, and never displaces anybody.
Only the display client's `MSG_INPUT` is honoured.

This is not fussiness, it is the fork's worst bug rebuilt correctly. From the
sl0ppi roadmap: `zellij action` failed 20-25%% of the time because every action
probed liveness by connecting, *that probe counted as a client*, and its
teardown removed the real one. We reproduced it exactly — `sl0ppty ls` kicked
the attached client off — within an hour of the server existing. The regression
test runs 20 x (`ls` + control command) against a live client and requires 0
failures and a client that can still type.

## Shape

```
sl0ppty-server  (one per session; holds ptys + lib-vt terminals + layout)
   ├── epoll: pty fds, client socket, control socket, signalfd, timerfd
   ├── pane[] = { pty fd, GhosttyTerminal, purpose, chrome, hit-list }
   ├── layout tree (tabs -> split tree -> panes; stack/rail are node modes)
   ├── compositor: dirty panes -> cell grid -> diff vs last frame -> ANSI
   └── control API: newline-delimited JSON on a unix socket
sl0ppty-client  (raw mode; decode input -> events; write bytes to tty)
```

Frame pacing: pty reads are coalesced and painted on a timerfd at a cap
(~120Hz), never per-read. That single property is why tmux feels sluggish under
`cat bigfile` and we will not.

## Milestones

- **M0** — pty + one fullscreen pane + input passthrough + resize ✅
- **M0.25** — input decoder + per-pane re-encode ✅ (pulled forward: retrofitting
  it under a layout tree would have been worse, and it is the load-bearing
  half of the mouse work in M4)
- **M0.5** — headless driver + screen-assert harness ✅
- **M1** — layout tree, splits, focus, frames (gap/padding/title alignment) ✅
- **M2** — server/client split, detach/reattach, control socket ✅
- **M3** — tabs, `purpose=`, JSON control API ✅
- **M4** — chrome: OSC 5577, buttons, hit-list mouse ✅ (drag-to-reorder deferred to M5)
- **M5** — built-in status bar, pane finder overlay, responsive (D6) ✅
- **M6** — config file: geometry, theme, keybindings, live reload ✅
- **M7** — layout files, suspended panes, `apply-layout` ✅ (what `sl0ppi up`
  is ported onto)
- **M8** — pane sizes as weights: keyboard resize, drag the gap to move a
  boundary, drag a title to swap two panes, `equalize` to put every share back
  to even, `rotate-layout` to turn the whole tab a quarter turn ✅
- **M9** — workspaces (D21): a project root scanned for projects, a third
  picker subject to go to one, layout files that are portable because a relative
  `cwd=` means the file's own directory, a dump that records what each pane is
  *running* rather than only what a layout told it to run, `save-workspace` to
  write one out, and a purpose you can set from the keyboard — the last two being
  what made a hand-arranged tab worth saving at all ✅

### The border is the button

The per-pane `+` is gone. It was one glyph for a verb with four directions, so
it always split into columns; it cost every frame three columns of title,
forever, for something pressed rarely; and there were N of them on screen for
one verb.

**Clicking a pane's border splits toward it.** The side you click is the side
the new pane appears on, which is the direction information a single glyph
could never carry. Hovering a border arms it (the edge goes heavy) and draws a
dashed line where the new boundary would land, so the gesture explains itself
and costs nothing when the pointer is elsewhere.

The top border keeps its other job: **click splits upward, drag moves the
pane**. A press that never moves is a click — the same distinction the drag
machine already had to make.

**The guide arms on dwell, not on contact** (`hover_delay_ms`, 250ms). A
pointer crossing a border on its way somewhere else should not make the screen
flash, and the timer is on *pointer stillness* rather than on the target, so
sweeping across three borders arms none of them. Holding the button on a
border is intent, and skips the wait.

That needs the event loop to wake when nothing has happened — the same
mechanism toasts already needed, since a pointer that is deliberately not
moving generates no events to repaint on.

**The guide is derived, not remembered.** Which border the pointer is over is
worked out *during the paint*, by asking the hit list that same paint just
filled. The first version remembered the answer from the last motion event,
which meant that immediately after a split the guide still described the
layout from before it, until the mouse moved and corrected itself. Same lesson
as the hit list, one level up: do not remember what you can derive from the
frame you are drawing.

### One drag machine, four verbs (M8)

Pane sizes are **weights**, so an even split is simply equal weights and
resizing is not a special case of anything: the layout pass stays the same
pure function of the tree and the rect, and a resized layout survives a
collapse/expand cycle for free.

**Rotating is a permutation of the tree** (`rotate-layout`, `C-a Space`): every split
changes axis, and a row split's children reverse — turn a column of panes
clockwise and the one that was on top is the one on the right, while a row's
leftmost becomes its top. That asymmetry is the whole feature; getting it wrong
gives a mirror image, which looks almost right until you notice four turns do
not come back. They do come back, exactly, which is what makes it safe to press
and why the inverse is three more turns rather than a second mirrored routine
with its own chance of being wrong. A turn is **refused when it would not fit**:
a share of the height is not always a share the width can afford, and a node
that cannot give its children their floor takes the whole tab down to a list
(D6) — so the turn is tried, the layout is asked what it made of it, and one
that flattened the tab is put back with a word. Same rule `split_fits` applies
to splitting, for the same reason: an action a person just asked for should not
be the thing that breaks the arrangement.

**Equalising is one pass over the weights** (`equalize`, `C-a =`), because the
weights are the whole state: no layout of its own, nothing remembered, and the
next frame is the answer. Each split's children are weighted by *how many
visible panes are behind each of them*, not given one weight each — one pane
beside a stack of two is a third of the width, not half of it, or "even" would
mean something different at every depth. Exactly equal cell counts are not
something a tree can always express (a nested split pays for its own gaps out
of its share, and a share that is not a whole number of cells lands where the
layout's remainder loop puts it), so the action promises the even *division*
the tree can express. Minimised panes are not counted: they are out of the
layout and cost a row in the strip rather than a share, so counting one would
hand its share to a pane nobody can see.

Every mouse verb starts from the hit list, so none of them can disagree with
what is on screen:

| target | painted by | drag does |
|---|---|---|
| a frame's top row | `draw_frame` | swap this pane with the one you drop it on |
| the gap between two children | `draw_node`, from the rects they were just given | move that boundary |
| a tab in the strip | `draw_tab_strip` | move it to where you drop it |
| ...and a *pane* dropped on a tab | `draw_tab_strip` again | move the pane into that tab; onto the `+`, into one of its own |

The fourth is the third one's target read the other way round, which is why it cost
almost nothing: the strip was already a row of drop targets and a pane in your hand
was already a thing being carried somewhere. `ptr_on` deliberately reports nothing
during a drag, so the strip draws the two drop states itself — every tab the pane
does not already live in as a candidate, the one under the pointer filled — which
are the states the panes were already using for the same question. A pane cannot be
dropped on the tab it is in, and that refusal is said out loud rather than looking
like a drag that failed to take.

The tab drag reorders **as you go** rather than dropping at the end. The strip
is the only thing that could show an insertion point, and a strip already
showing the result needs no such invention -- so there is no ghost tab, no
caret, and nothing remembered between frames. Dragging off the strip is not a
cancel: it is simply not a place to put a tab, so it stays where it last was.
`cur` is a *position*, so a move has to recompute it; carrying it across would
leave you looking at whichever tab slid into the index you used to be at.

The drop target is highlighted while dragging, and **any keystroke ends a
drag** — a release can go missing (the pointer leaves the terminal, the client
detaches mid-drag) and a mouse wedged in an invisible state is the worst
possible outcome.

M4 landed: the pi extensions' exact byte patterns are an acceptance test
(`tests/test_osc5577.py::test_pi_extension_compat`), including `buttons` with
no payload — answer-picker's way of dropping buttons while keeping the status
text — and a click report that satisfies the extension's own `CLICK_RE`.

Three protocol details worth keeping in mind, all found by testing:

- **An over-long button id must be rejected, not truncated.** Unescaping a
  40-character id into a 33-byte buffer produced a *valid* 32-character id, so
  a hostile pane could mint one that collides with an id its program already
  trusts. The id is now unescaped into a large buffer and validated at full
  length.
- **BEL terminates an OSC just as ST does**, which matters more than it looks:
  a trailing `ESC \` inside a double-quoted shell string escapes the quote, so
  anything scripting this from a shell will reach for BEL.
- **A reply must never re-parse as a request.** Every message the session sends
  a program ends its verb in `-reply`, and nothing dispatches one. `hello` used
  to answer with the verb `hello`, so a pane that echoed what it was sent — a
  shell with echo on, `cat`, a REPL waiting for a line — answered the answer:
  measured at 4.3 MB and 165,532 hellos in a second and a half. Fixed as the
  rule rather than the instance, because the loop is a property of "a reply that
  looks like a request" and the next verb to get an answer would have found it
  again.

**D13, revisited: a program may set its own pane's shader chains**, behind
`in_band_shaders` (off by default). The original reasoning stands — a program
that can restyle the session it is running in is a hazard, and `cat
hostile.txt` must not be able to dim your panes — but *impossible* was the wrong
shape for it: arriving at a colour pass by editing a file, saving, looking and
guessing again is slow enough that shaders go unwritten. So it is possible, it
costs a line of consent, and it is scoped to the pane that asked: its own two
chains, never its neighbour's and never what the config said about everybody
else. The payload is the config's own chain syntax parsed by the same function
(`config_parse_chain`), because a chain that meant one thing typed and another
thing pasted would be worse than not having it. `contrib/shader-repl` is the
prompt this exists for, and `:paste` prints the block to keep.

## Non-goals

- Not a zellij/tmux replacement for anyone but us.
- No multi-user session rendering (D10).
- No content resurrection. Layout restore yes, replaying scrollback no.
- No plugin sandbox (D9, D15): a shader plugin is native code with the
  session's privileges.
- Nothing but `x86_64-linux` until it works.
