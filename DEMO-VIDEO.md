# slosh — demo video script (social media cut)

Concept for a ~75s feature-tour video. Every scene is a single visual beat:
one action, one payoff, readable from a phone. No scene depends on reading
terminal text — the terminal is the *picture*, big captions carry the words.

## Ground rules for the recording

- **Font huge.** Record at roughly **80×22 cells** (or fewer). If a normal
  demo gif is 200 cols, this is not that video. Titles, borders and the
  mini-pane strip must be legible at phone width.
- **16:9 master, 1:1-safe center.** Keep the interesting pane in the middle
  60% so a square crop for feeds still works.
- **One action per scene, 3–8 seconds each.** Cut on the payoff, not after it.
- **Captions are overlaid in post** (big type, 4–6 words), never read off the
  screen. Terminal text is texture; captions are the message.
- **Slow the mouse down.** Deliberate, straight-line movements. A cursor
  highlight ring helps.
- **A high-contrast theme** and colourful pane content (e.g. `btop`, a log
  tail with colour, a build with warnings) so shader scenes visibly *do*
  something.
- Use a staged home dir with 4–6 fake projects under `project_roots` so the
  picker looks lived-in.
- Music-driven; no voiceover required. Keep a voiceover-optional line per
  scene anyway (included below) in case a narrated cut performs better.

---

## Scene list

### 1. Cold open — the mouse hook (0:00–0:06)

The single most demoable thing slosh has. Start on **one full-screen pane**
running something alive (colourful log scroll).

- Move the mouse to the **middle of the right edge**, click → pane splits
  toward the click.
- Immediately click the middle of the new pane's bottom border → splits again.
- **Drag a gap** to move a boundary. **Drag a pane by its title** onto another
  → they swap.

> Caption: **"a terminal multiplexer where the mouse actually works"**
> VO: "Click a border to split. Drag a gap to resize. Drag a title to swap."

*Why first: it's motion-rich, needs zero explanation, and instantly separates
slosh from tmux in the first 3 seconds — the scroll-stopper.*

### 2. Smart splits & instant tidy (0:06–0:12)

Keyboard beat, still fast.

- `C-a Enter` twice — splits pick the sensible axis themselves.
- Layout is now uneven → `C-a 0` → everything snaps to even shares.
- `C-a Space` → the **whole layout rotates a quarter turn**. Press it twice
  more quickly — it's a great visual.

> Caption: **"splits that pick the right axis · one key to tidy · rotate the whole tab"**

### 3. Zoom & the minimise strip (0:12–0:19)

- `C-a z` → focused pane fills the tab. `C-a z` → back.
- `C-a m` on two panes → they shrink into the **strip of miniature pane
  frames along the bottom**, still running (one should be visibly ticking,
  e.g. a timer/log).
- Click a miniature → it pops back into the layout.

> Caption: **"zoom it · or put it away — still running"**

### 4. Floating panes (0:19–0:25)

- `C-a f` → pane lifts above the layout; nudge it around with shift+arrows,
  `=` to grow it about its centre.
- `C-a F` → a fresh **throwaway floating shell** appears; type one quick
  command; close it.

> Caption: **"float a pane · or summon a throwaway shell"**

### 5. A pane outlives its command (0:25–0:33)

The philosophical differentiator, shown not told.

- A pane is running a build/test command. It **exits** — the output stays,
  the frame shows the exit status and **`[re-run]` `[close]`** buttons.
- Click `[re-run]` → the same command runs again *in the same pane*, previous
  run still above it.

> Caption: **"a pane outlives its command — output stays, one click to re-run"**

### 6. Die and come back (0:33–0:40)

- Kill the terminal window mid-scroll (or cut the fake ssh connection —
  show a window closing hard).
- New terminal: `slosh` → **exact same layout, everything still running**,
  scrollback intact.
- Bonus beat: shrink the window very small → tab collapses to a **list of
  pane headers**; grow it back → the layout returns exactly.

> Caption: **"detach, disconnect, resize — the session doesn't care"**

### 7. Projects: `C-a w` (0:40–0:49)

- `C-a w` → the **projects picker** over the staged roots (mix of declared
  and `.git · no layout` entries).
- Type 2–3 letters, Enter → a tab **arrives fully arranged**: editor pane,
  shell, and a **suspended** dev-server pane.
- `C-a r` on the suspended pane → the server starts.
- Quick flash: `C-a W` → "layout saved" toast — the arrangement is now
  checked into the project.

> Caption: **"open a project — it arrives already arranged"**
> VO: "The layout lives in the repo. Suspended panes don't run until you say so."

### 8. Shaders, live (0:49–0:59)

The eye-candy scene — this is the clip people share.

- `C-a e` → config opens in a pane, terminal visible beside it.
- Type into `shaders { }`, **save, watch it apply instantly** (hot reload):
  - `vignette amount=90` → edges darken across every pane
  - `ruler at=80 color="#7aa2f7"` → column ruler appears in the editor
  - `spotlight radius=12` → brightness follows the cursor as you move it
- One expression for the finale:
  `dim amount="(y % 2) * 40"` → scanlines sweep in.
- Trigger a bell in another pane → its **border flashes** (chrome shader).

> Caption: **"colour passes over cells — written as expressions, applied on save"**

### 9. Scriptable to the same depth (0:59–1:08)

Split screen feel: a plain shell on the left, slosh on the right.

- Left: run one visible JSON one-liner:
  `slosh -s demo cmd '{"cmd":"apply-layout","kdl":"…"}'`
  → right side: a whole tab of panes **materialises**.
- Then, inside a pane, a fake "agent" script runs:
  `printf '\e]5577;1;status;building 3/7\e\\'` → a **status appears in the
  pane's frame**, then two **real clickable buttons** (`Approve` /
  `Cancel`). Click Approve → the script visibly reacts.

> Caption: **"everything the keyboard does, the socket does — and a pane can grow its own buttons"**
> VO: "One JSON object per line. Your agent doesn't scrape the screen — it asks."

### 10. Closer (1:08–1:15)

- `C-a ?` → cheatsheet flashes up for a beat (proof there's more).
- Cut to a clean shell:
  ```
  make          # ~1 second, builds one static binary
  slosh
  ```
- End card: logo (from `logo.txt`), repo URL, one line:

> Caption: **"slosh — panes, tabs, sessions. one static binary."**

---

## Alternate cuts

- **15s teaser** (for the algorithm): Scene 1 + rotate from Scene 2 +
  spotlight/scanlines from Scene 8 + end card. Pure motion, zero reading.
- **30s cut**: Scenes 1, 3, 6, 8, closer.
- Scenes are deliberately self-contained so they can be reordered or posted
  individually as follow-ups ("did you know a slosh pane can…").

## Production checklist

- [ ] Staged demo home: theme, config with `project_roots`, 4–6 fake projects
      (one with `slosh.layout`, others `.git`-only), colourful long-running
      commands (log generator, `btop`, a build that exits with warnings)
- [ ] Record at ~80×22, large font; check legibility on a phone before
      recording everything
- [ ] Cursor highlight for mouse scenes; rehearse the drag paths
- [ ] Scene 6 needs two takes stitched (kill window / reattach)
- [ ] Scene 9 needs the small "agent" script written beforehand (status →
      buttons → react to click on stdin)
- [ ] Captions in post: max 6 words, on-brand type, safe for 1:1 crop
- [ ] Export: 1080p master, plus 1:1 and 9:16 crops for feeds/shorts
