# recordings — small in-browser playbacks of real slosh features

The front page will intersperse its copy with playable recordings, the way
pi.dev does with asciinema casts. Ours are **not** asciinema: the format and
the player are native, because slosh already did the hard part. This
directory holds the machinery; this file holds the plan and its status.

## The two decisions that shape the rest

**1. Recordings are programs, not performances.** pi.dev's casts are humans
typing into ghostty under `asciinema rec` — their hero cast is 7.7 MB, and
re-recording after a UI change means a human re-typing the take. Ours are
driver scripts against `slosh --script` (through `tests/harness.py`, the same
Session the test suite uses), with a **virtual clock**: pauses and typing
cadence are authored, jitter is seeded, so the same script yields the same
file byte for byte on any machine. A UI change re-renders every demo with
`make`; a stale demo is impossible — the same argument as `tests/test_skill.py`.

**2a. Borders are geometry, not glyphs.** Box-drawing, block and shade
characters are never left to the font (a font glyph is hinted against its own
metrics: a column of │ shows seams, a scaled ╭ goes jagged). The player
paints them on a canvas overlay as device-pixel geometry with rounded,
SHARED cell boundaries — adjacent cells tile exactly — while the real
character stays in the DOM as transparent ghost text, so selection and copy
still carry it. Arms/junction weights, tangent-aligned quarter-ellipse arcs
for ╭╮╯╰, dashes and block/shade rects are ported from shellglass's viewer
(~/dev/sl0pday/vendor/shellglass/viewer/viewer.ts), which solved this for
sl0p.foo. Two more rules learned there: metrics are pinned in pixels (row
height = --sc-lh), and the player scales by choosing a font size — never a
CSS transform, whose per-span resampling was the other source of jag.

**2b. No VT in the browser.** `snapshot` hands back the *composited* screen:
text rows plus style runs with colours already resolved to hex by the theme.
So the file format is styled rows diffed per frame, and the player is ~200
lines of dependency-free JS doing innerHTML on the rows that changed. No
asciinema-player, no wasm, no xterm.js, no palette mapping, nothing fetched —
which is the site's founding rule. Text in a playing demo is real text:
selectable, copyable, findable.

Measured, hero demo (26 s, 159 events): **81 KiB raw, 2.9 KiB gzipped**.

## What exists (working end to end)

| file | what |
|---|---|
| `castgen.py` | Recorder: virtual clock, per-action capture, row-level diffs, the sloshcast writer. Format documented in its docstring |
| `demos/panes.py` | the opener: chord split, border-click split, gap drag, title-drag swap, zoom, a tab and back |
| `demos/sessions.py` | TWO live sessions (`main` sl0p-pink, `scratch` phosphor-green) with authored bare-shell frames between: attach, detach, hop, return — and the pane that died off camera kept its epitaph, [re-run] clicked |
| `demos/reflow.py` | quarter-turn, shrink to header list, grow back: the arrangement returns |
| `demos/config.py` | C-a e opens the config in a vim pane: theme flip by substitution, `padding`, `compact true` — each landing on save |
| `demos/shaders.py` | contrib/shader-repl: chrome tint, cursor spotlight, ruler; a bell rings unfocused and the frame flashes |
| `demos/scripting.py` | left: socket JSON typed with verbatim replies while the session performs each verb; right: deploy.py draws status + [Approve] into its frame, the pointer's click lands on its stdin |

One cast per tour step, one claim per step, 2–4 proofs per cast — and the
step's bullet list on the page is the cast's beat list, in order. The tour
closes on a CTA step (install / try in the browser) with the last cast still
playing. Multi-session recordings work by `Recorder.switch()`ing between
live Sessions plus `shell()`/`shell_type()` authored interstitial frames
(`[detached]`, a prompt, typing the next attach).
| `player.js` | the player: `mountSloshcast(el, data, opts)` + declarative `<div data-sloshcast="x.json" data-loop data-autoplay data-poster="12.5">` mounts. Poster frame, click to play/pause, loop, `seek()`, scale-to-fit (the webdemo's CSS-transform trick) |
| `player.css` | the chrome: frame, play glyph, title/time bar. System monospace stack, per the site's rules |
| `test.html` | eyeball page (`make serve`) |
| `seek.html` | `?t=SECONDS` renders the demo frozen at that moment — for screenshots and for picking `data-poster` times |
| `Makefile` | `demos/*.py` → `build/*.json`; `make serve`; `make clean` |

The format ("sloshcast v1", one JSON file) is defined in `castgen.py`:
header (`cols rows fg bg title dur`) + events (`t`, changed `rows` as
`[text, [[x,w,fg,bg,attrbits],…]]`, `cur`, optional `size` for mid-demo
resizes, which carry a full frame). Event 0 is the full first frame, so a
loop restart or a `seek(0)` is cheap. attrbits: 1 bold, 2 italic,
4 underline, 8 dim, 16 reverse, 32 strike.

Verified: rendering against headless chromium screenshots (panes, truecolor
runs, cursor, focus colours, pointer, hover hints, compact-mode headers all
correct), and playback over CDP — clock advances, events land, the loop
wraps.

**The pointer track is in.** `Recorder` grows `move_to` / `press` / `release`
/ `click` / `drag_to`: real SGR mouse bytes go to slosh (hover hints and drop
zones are the program's own), while `"ptr"` events record the cell so the
player can draw the arrow a screen capture never shows. Pink while pressed;
the CSS transition does the glide.

**Time inside slosh is real.** A toast expires and a shader animates on the
monotonic clock, so `pause()` asks the session's `deadline` and, when a next
frame is wanted within the pause, waits that long for real and samples it.
Toasts fade mid-pause exactly as they would live; animations get captured at
the cadence they asked for; a demo with neither still renders instantly.
Rendering all three demos: ~7 s.

One gotcha worth keeping: the JSON `send` verb takes bytes **verbatim**
(JSON strings carry control characters natively); the `\x01` escape
mini-language belongs to the bare-verb form only. `castgen.py` sends real
bytes.

## Still to do

**The shot list is done** — every `#different` step has a cast and swaps on
scroll. Lessons that cost time, so they stay written down:

- The in-band `shader-reply` races a shell's `stty`: slosh answers on stdin
  faster than a fork+exec, so a bare shell paints the reply into its prompt.
  `contrib/shader-repl` exists precisely to hold that conversation — type at
  it, not at `sh`.
- A bell must ring *unfocused* to mark and flash; give it a fuse (a `sleep`
  in the pane's script) and use `Recorder.wall()` for the real seconds.
- Commanded panes come from layouts; a command that is a file (`sh checks`)
  keeps the dead frame's `[ran: …]` line readable.

The detach story is in (`sessions.py`); what remains is pacing polish once
the page has been scrolled by real hands for a while.

**Front-page integration.** Done as a scrolly feature tour: `#different` is
a two-column grid — a `position: sticky` stage pinning the terminal
v-centred on the left, the six feature steps scrolling by on the right. An
IntersectionObserver with a `-45%` root margin makes the step under the
viewport middle active; its `data-cast` picks the stage's recording
(restart + play, others pause). Steps whose recording is not scripted yet
have no `data-cast` and keep the current one playing. A second observer
pauses everything while the tour is off screen; `prefers-reduced-motion`
gets posters only. On one column the sticky moves up to `.scrolly-stage`
(in block layout `.stage-pin`'s parent is only as tall as the player) and
the terminal rides under the nav. The hero cast replaced the placeholder
mp4 in `#video`.

**Still open from that:** the no-JS story — render frame 0 server-side into
static poster HTML at build time (same row→spans logic, in Python) so the
page shows a real first frame before, or without, JavaScript.

**Build wiring.** Done: `web/build-site` (FULL=1) runs
`make -C web/recordings` and copies casts + player to
`build/www/recordings/`. Casts ship raw; the host's gzip does the rest
(2.9 KiB on the wire for the hero).

**Polish, later, maybe.** A scrub bar (seek() exists), speed control,
`document.hidden` pausing autoplay loops, a `make check` that replays every
cast headlessly and diffs the final frame against the demo's last snapshot.
Known wrinkle: frames sampled during a live animation (a border flash) take
their colour from real elapsed time, so those few frames are not byte-
deterministic across renders; everything else is.
