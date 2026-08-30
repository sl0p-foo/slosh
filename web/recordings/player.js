// sloshcast player -- replays slosh's own composited frames into a DOM grid.
//
// No VT, no wasm, no dependency: the generator (castgen.py) already asked the
// compositor for styled rows with colours resolved to hex, so playing one back
// is innerHTML on the rows that changed. The whole player is this file.
//
// Box-drawing, block and shade characters are NOT left to the font. A font
// glyph is hinted against its own metrics, so a column of │ shows seams and a
// scaled ╭ goes jagged. Instead -- the approach learned from shellglass's
// viewer -- a canvas overlay paints those cells as device-pixel geometry with
// ROUNDED, SHARED cell boundaries (adjacent cells tile exactly), while the
// real character stays in the DOM as transparent "ghost" text so selection
// and copy still work. For the same reason the player never transform-scales:
// it fits by choosing the font size, and pins the row height in pixels.
//
//   mountSloshcast(el, data, {loop, autoplay, speed})
//
// `data` is the parsed JSON; `el` becomes the player. Returns {play, pause,
// toggle, restart, seek, fit}.

"use strict";

// ── glyph geometry (after shellglass viewer.ts) ──────────────────────────────

// Arm weights "urdl" (0 none, 1 light, 2 heavy) for U+2500-257F; "0000" = not
// arms-coverable here (doubles/diagonals stay on the font path, dashes and
// arcs have their own ops below).
const SC_ARMS =
  "0101020210102020" + // 2500 ─ ━ │ ┃
  "0000000000000000" + "0000000000000000" + // 2504-250B dashes (dashesOps)
  "0110021001200220" + // 250C ┌┍┎┏
  "0011001200210022" + // 2510 ┐┑┒┓
  "1100120021002200" + // 2514 └┕┖┗
  "1001100220012002" + // 2518 ┘┙┚┛
  "1110121021101120" + // 251C ├┝┞┟
  "2120221012202220" + // 2520 ┠┡┢┣
  "1011101220111021" + // 2524 ┤┥┦┧
  "2021201210222022" + // 2528 ┨┩┪┫
  "0111011202110212" + // 252C ┬┭┮┯
  "0121012202210222" + // 2530 ┰┱┲┳
  "1101110212011202" + // 2534 ┴┵┶┷
  "2101210222012202" + // 2538 ┸┹┺┻
  "1111111212111212" + // 253C ┼┽┾┿
  "2111112121212112" + // 2540 ╀╁╂╃
  "2211112212212212" + // 2544 ╄╅╆╇
  "1222212222212222" + // 2548 ╈╉╊╋
  "0000000000000000" + // 254C-254F dashes (dashesOps)
  "0000000000000000".repeat(8) + // 2550-256C doubles (font)
  // 256D-2570 arcs (arcOps), 2571-2573 diagonals (font)
  "0001100001000010" + // 2574 ╴╵╶╷
  "0002200002000020" + // 2578 ╸╹╺╻
  "0201102001022010"; //  257C ╼╽╾╿

function scBoxArms(cp) {
  if (cp < 0x2500 || cp > 0x257f) return null;
  const o = (cp - 0x2500) * 4;
  const u = +SC_ARMS[o], r = +SC_ARMS[o + 1], d = +SC_ARMS[o + 2], l = +SC_ARMS[o + 3];
  return u || r || d || l ? [u, r, d, l] : null;
}

const scLw = (weight, light) => (weight === 2 ? 2 * light : light);

// Arms (lines, corners, tees, crosses): each arm its own weight, extended past
// centre by the crossing bar's half-extent so the junction fills solid.
function scArmsOps(x0, y0, x1, y1, arms, light) {
  const [u, r, d, l] = arms;
  const midX = Math.round((x0 + x1) / 2);
  const midY = Math.round((y0 + y1) / 2);
  const vh = scLw(Math.max(u, d), light) >> 1;
  const hh = scLw(Math.max(l, r), light) >> 1;
  const ops = [];
  if (u) { const t = scLw(u, light); ops.push([midX - (t >> 1), y0, t, midY + hh - y0]); }
  if (d) { const t = scLw(d, light); ops.push([midX - (t >> 1), midY - hh, t, y1 - (midY - hh)]); }
  if (l) { const t = scLw(l, light); ops.push([x0, midY - (t >> 1), midX + vh - x0, t]); }
  if (r) { const t = scLw(r, light); ops.push([midX - vh, midY - (t >> 1), x1 - (midX - vh), t]); }
  return { rects: ops };
}

function scDashesOps(x0, y0, x1, y1, cp, light) {
  let horiz, n, weight;
  if (cp <= 0x250b) {
    const k = cp - 0x2504;
    horiz = (k & 3) < 2;
    n = k < 4 ? 3 : 4;
    weight = k & 1 ? 2 : 1;
  } else {
    const k = cp - 0x254c;
    horiz = k < 2;
    n = 2;
    weight = k & 1 ? 2 : 1;
  }
  const t = scLw(weight, light);
  const midX = Math.round((x0 + x1) / 2);
  const midY = Math.round((y0 + y1) / 2);
  const rects = [];
  for (let i = 0; i < n; i++) {
    const s0 = (i + 0.2) / n, s1 = (i + 0.8) / n;
    if (horiz) {
      const a = Math.round(x0 + s0 * (x1 - x0)), b = Math.round(x0 + s1 * (x1 - x0));
      rects.push([a, midY - (t >> 1), b - a, t]);
    } else {
      const a = Math.round(y0 + s0 * (y1 - y0)), b = Math.round(y0 + s1 * (y1 - y0));
      rects.push([midX - (t >> 1), a, t, b - a]);
    }
  }
  return { rects };
}

// Rounded corners ╭╮╯╰: a quarter ellipse from the far corner whose tangent
// points land on the straight arms' centrelines, so it meets ─/│ flush.
function scArcOps(x0, y0, x1, y1, cp, light) {
  const off = (light % 2) / 2;
  const mx = Math.round((x0 + x1) / 2) + off;
  const my = Math.round((y0 + y1) / 2) + off;
  const corners = [[x1, y1], [x0, y1], [x0, y0], [x1, y0]]; // ╭ ╮ ╯ ╰
  const angles = [
    [Math.PI, 1.5 * Math.PI], [1.5 * Math.PI, 2 * Math.PI],
    [0, 0.5 * Math.PI], [0.5 * Math.PI, Math.PI],
  ];
  const [cx, cy] = corners[cp - 0x256d];
  const [a0, a1] = angles[cp - 0x256d];
  return { arc: [cx, cy, Math.abs(cx - mx), Math.abs(cy - my), a0, a1, light] };
}

// Block elements: solid rects (halves/eighths/quadrants) and alpha shades.
const SC_QUADRANTS = [4, 8, 1, 13, 9, 7, 11, 2, 6, 14]; // 2596-259F
function scBlockOps(x0, y0, x1, y1, cp) {
  const W = x1 - x0, H = y1 - y0;
  const R = (u0, v0, u1, v1, alpha) => {
    const a = Math.round(x0 + u0 * W), b = Math.round(x0 + u1 * W);
    const c = Math.round(y0 + v0 * H), d = Math.round(y0 + v1 * H);
    return [a, c, b - a, d - c, alpha];
  };
  if (cp === 0x2580) return { rects: [R(0, 0, 1, 0.5)] };
  if (cp >= 0x2581 && cp <= 0x2588) return { rects: [R(0, 1 - (cp - 0x2580) / 8, 1, 1)] };
  if (cp >= 0x2589 && cp <= 0x258f) return { rects: [R(0, 0, (0x2590 - cp) / 8, 1)] };
  if (cp === 0x2590) return { rects: [R(0.5, 0, 1, 1)] };
  if (cp <= 0x2593) return { rects: [R(0, 0, 1, 1, (cp - 0x2590) / 4)] };
  if (cp === 0x2594) return { rects: [R(0, 0, 1, 0.125)] };
  if (cp === 0x2595) return { rects: [R(0.875, 0, 1, 1)] };
  const m = SC_QUADRANTS[cp - 0x2596];
  const rects = [];
  if (m & 1) rects.push(R(0, 0, 0.5, 0.5));
  if (m & 2) rects.push(R(0.5, 0, 1, 0.5));
  if (m & 4) rects.push(R(0, 0.5, 0.5, 1));
  if (m & 8) rects.push(R(0.5, 0.5, 1, 1));
  return { rects };
}

function scGlyphOps(cp, x0, y0, x1, y1, light) {
  if (cp >= 0x2580 && cp <= 0x259f) return scBlockOps(x0, y0, x1, y1, cp);
  if ((cp >= 0x2504 && cp <= 0x250b) || (cp >= 0x254c && cp <= 0x254f))
    return scDashesOps(x0, y0, x1, y1, cp, light);
  if (cp >= 0x256d && cp <= 0x2570) return scArcOps(x0, y0, x1, y1, cp, light);
  const arms = scBoxArms(cp);
  return arms ? scArmsOps(x0, y0, x1, y1, arms, light) : null;
}

// The cells the canvas paints (must agree with scGlyphOps): geometry replaces
// the glyph, the DOM keeps it as transparent ghost text.
function scIsCanvasCp(cp) {
  if (cp >= 0x2580 && cp <= 0x259f) return true;
  if (cp < 0x2500 || cp > 0x257f) return false;
  if ((cp >= 0x2504 && cp <= 0x250b) || (cp >= 0x254c && cp <= 0x254f)) return true;
  if (cp >= 0x256d && cp <= 0x2570) return true;
  return scBoxArms(cp) !== null;
}

// ── the player ────────────────────────────────────────────────────────────────

function mountSloshcast(el, data, opts = {}) {
  const speed = opts.speed || 1;
  const PAD = 8; // .sc-screen padding, px
  const BASE_FONT = 14;
  const esc = (s) =>
    s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");

  // -- one row: text + [x, w, fg, bg, attrbits] runs -> spans ------------
  // Canvas-covered characters become ghost spans: transparent text under the
  // geometry, so what you select and copy is still the real characters.
  function ghostHtml(s) {
    let out = "", plain = "";
    for (const ch of s) {
      if (scIsCanvasCp(ch.codePointAt(0))) {
        if (plain) { out += esc(plain); plain = ""; }
        out += `<span class="sc-g">${esc(ch)}</span>`;
      } else plain += ch;
    }
    return out + esc(plain);
  }

  function rowHtml(row) {
    const [text, runs] = row;
    let out = "", x = 0;
    for (const [rx, w, fg, bg, at] of runs) {
      if (rx > x) out += ghostHtml(text.slice(x, rx));
      let st = "";
      if (at & 16) {
        st += `color:${bg || data.bg};background:${fg || data.fg};`;
      } else {
        if (fg) st += `color:${fg};`;
        if (bg) st += `background:${bg};`;
      }
      if (at & 1) st += "font-weight:bold;";
      if (at & 2) st += "font-style:italic;";
      if (at & 4) st += "text-decoration:underline;";
      if (at & 8) st += "opacity:.55;";
      if (at & 32) st += "text-decoration:line-through;";
      out += `<span style="${st}">${ghostHtml(text.slice(rx, rx + w))}</span>`;
      x = rx + w;
    }
    return out + ghostHtml(text.slice(x));
  }

  // -- the grid ----------------------------------------------------------
  el.classList.add("sloshcast");
  el.style.background = data.bg;
  el.innerHTML =
    `<div class="sc-frame"><pre class="sc-screen"><canvas class="sc-canvas"></canvas>` +
    `<span class="sc-cursor"></span><span class="sc-pointer"></span></pre>` +
    `<button class="sc-play" aria-label="Play">▶</button></div>` +
    `<div class="sc-bar"><span class="sc-title">${esc(data.title || "")}</span>` +
    `<span class="sc-time"></span></div>`;
  const frame = el.querySelector(".sc-frame");
  const screen = el.querySelector(".sc-screen");
  const canvas = el.querySelector(".sc-canvas");
  const cursor = el.querySelector(".sc-cursor");
  const pointer = el.querySelector(".sc-pointer");
  const bigPlay = el.querySelector(".sc-play");
  const timeEl = el.querySelector(".sc-time");
  const ctx = canvas.getContext("2d");
  screen.style.color = data.fg;

  let rowEls = [];
  let rowsData = []; // retained [text, runs] per row: the canvas reads it
  let gcols = data.cols, grows = data.rows;
  let cellW = 8, cellH = 17, dpr = 1;
  let canvasDirty = false;

  function grid(cols, rows) {
    gcols = cols;
    grows = rows;
    rowsData = [];
    for (const d of screen.querySelectorAll(".sc-row")) d.remove();
    rowEls = [];
    for (let y = 0; y < rows; y++) {
      const d = document.createElement("div");
      d.className = "sc-row";
      d.innerHTML = "\u00a0";
      screen.appendChild(d);
      rowEls.push(d);
    }
    fit();
  }

  function apply(ev) {
    if (ev.size) grid(ev.size[0], ev.size[1]);
    if (ev.rows)
      for (const y in ev.rows)
        if (rowEls[y]) {
          rowsData[y] = ev.rows[y];
          rowEls[y].innerHTML = rowHtml(ev.rows[y]) || "\u00a0";
        }
    if ("cur" in ev) {
      if (ev.cur) {
        cursor.style.display = "block";
        cursor.style.left = ev.cur[0] + "ch";
        cursor.style.top = rowEls[ev.cur[1]]
          ? rowEls[ev.cur[1]].offsetTop + "px"
          : "0";
      } else cursor.style.display = "none";
    }
    if ("ptr" in ev) {
      if (ev.ptr) {
        pointer.style.display = "block";
        pointer.style.left = ev.ptr[0] + 0.5 + "ch";
        pointer.style.top = rowEls[ev.ptr[1]]
          ? rowEls[ev.ptr[1]].offsetTop + "px"
          : "0";
        pointer.classList.toggle("sc-pressed", !!ev.ptr[2]);
      } else pointer.style.display = "none";
    }
    canvasDirty = true;
  }

  // -- the canvas overlay: box/block glyphs as device-pixel geometry ------
  function cellRect(x, y) {
    return [
      Math.round(x * cellW * dpr),
      Math.round(y * cellH * dpr),
      Math.round((x + 1) * cellW * dpr),
      Math.round((y + 1) * cellH * dpr),
    ];
  }

  function repaintCanvas() {
    canvasDirty = false;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    const light = Math.max(1, Math.round(dpr));
    for (let y = 0; y < grows; y++) {
      const rd = rowsData[y];
      if (!rd) continue;
      const [text, runs] = rd;
      for (let x = 0; x < text.length; x++) {
        const cp = text.codePointAt(x);
        if (cp < 0x2500 || cp > 0x259f) continue;
        const [x0, y0, x1, y1] = cellRect(x, y);
        const ops = scGlyphOps(cp, x0, y0, x1, y1, light);
        if (!ops) continue;
        let fg = null;
        for (const r of runs)
          if (r[0] <= x && x < r[0] + r[1]) { fg = r[4] & 16 ? r[3] : r[2]; break; }
        ctx.fillStyle = ctx.strokeStyle = fg || data.fg;
        if (ops.rects)
          for (const [rx, ry, rw, rh, alpha] of ops.rects) {
            if (alpha !== undefined) ctx.globalAlpha = alpha;
            ctx.fillRect(rx, ry, rw, rh);
            if (alpha !== undefined) ctx.globalAlpha = 1;
          }
        if (ops.arc) {
          const [acx, acy, arx, ary, a0, a1, lw] = ops.arc;
          ctx.beginPath();
          ctx.lineWidth = lw;
          ctx.ellipse(acx, acy, arx, ary, 0, a0, a1);
          ctx.stroke();
        }
      }
    }
  }

  function flush() {
    if (canvasDirty) repaintCanvas();
  }

  // -- fit: choose a font size (never a transform), pin px metrics --------
  // A transform resamples the raster and every span rounds on its own, which
  // is exactly where jagged borders come from; a font size change re-hints.
  const probe = document.createElement("span");
  probe.style.cssText = "position:absolute;visibility:hidden;white-space:pre";
  probe.textContent = "M".repeat(32);

  function metricsAt(px) {
    screen.style.fontSize = px + "px";
    screen.appendChild(probe);
    const w = probe.getBoundingClientRect().width / 32;
    probe.remove();
    return w;
  }

  function fit() {
    dpr = window.devicePixelRatio || 1;
    const availHost = el.parentElement ? el.parentElement.clientWidth : 0;
    const avail = Math.max(120, availHost - 2) - 2 * PAD;
    let f = BASE_FONT;
    cellW = metricsAt(f);
    if (gcols * cellW > avail) {
      f = Math.max(6, Math.floor(((BASE_FONT * avail) / (gcols * cellW)) * 2) / 2);
      cellW = metricsAt(f);
      while (gcols * cellW > avail && f > 6) {
        f -= 0.5;
        cellW = metricsAt(f);
      }
    }
    cellH = Math.round(f * 1.25);
    screen.style.setProperty("--sc-lh", cellH + "px");
    screen.style.width = gcols * cellW + "px";
    canvas.width = Math.round(gcols * cellW * dpr);
    canvas.height = Math.round(grows * cellH * dpr);
    canvas.style.width = gcols * cellW + "px";
    canvas.style.height = grows * cellH + "px";
    el.style.width = Math.ceil(gcols * cellW) + 2 * PAD + "px";
    canvasDirty = true;
    flush();
  }
  addEventListener("resize", fit);

  // -- the clock ---------------------------------------------------------
  // Events are sparse (nothing changes between keystrokes), so instead of a
  // 60fps tick the player sleeps until the next event is due. performance.now
  // keeps it honest across throttled or background tabs: waking late just
  // applies everything that became due.
  let idx = 0,
    t = 0,
    t0 = 0,
    playing = false,
    timer = 0;

  function restart() {
    seek(0);
  }
  function seek(to) {
    idx = 0;
    t = to;
    pointer.style.display = "none";
    grid(data.cols, data.rows);
    while (idx < data.events.length && data.events[idx].t <= to)
      apply(data.events[idx++]);
    if (idx === 0 && data.events.length) apply(data.events[idx++]);
    flush();
    clock();
    if (playing) t0 = performance.now() - (t * 1000) / speed;
  }
  function clock() {
    const m = Math.floor(t / 60),
      s = Math.floor(t % 60);
    timeEl.textContent =
      `${m}:${String(s).padStart(2, "0")} / ` +
      `${Math.floor(data.dur / 60)}:${String(Math.floor(data.dur % 60)).padStart(2, "0")}`;
  }
  function step() {
    if (!playing) return;
    t = ((performance.now() - t0) / 1000) * speed;
    while (idx < data.events.length && data.events[idx].t <= t)
      apply(data.events[idx++]);
    flush();
    clock();
    if (t >= data.dur) {
      if (!opts.loop) return pause();
      restart();
      t0 = performance.now();
    }
    schedule();
  }
  function schedule() {
    const next = idx < data.events.length ? data.events[idx].t : data.dur;
    timer = setTimeout(step, Math.max(0, ((next - t) * 1000) / speed) + 1);
  }
  function play() {
    if (playing) return;
    playing = true;
    el.classList.add("sc-playing");
    if (idx >= data.events.length && t >= data.dur) restart();
    t0 = performance.now() - (t * 1000) / speed;
    schedule();
  }
  function pause() {
    playing = false;
    el.classList.remove("sc-playing");
    clearTimeout(timer);
  }
  const toggle = () => (playing ? pause() : play());
  frame.addEventListener("click", toggle);

  // -- poster: the first frame, standing still ---------------------------
  restart();
  if (opts.autoplay) play();
  return { play, pause, toggle, restart, seek, fit };
}

// Declarative mounts: <div data-sloshcast="hero.json" data-loop data-autoplay
// data-poster="12.5"> -- poster picks the paused frame shown before play.
for (const el of document.querySelectorAll("[data-sloshcast]")) {
  fetch(el.dataset.sloshcast)
    .then((r) => r.json())
    .then((data) => {
      const ctl = mountSloshcast(el, data, {
        loop: "loop" in el.dataset,
        autoplay: "autoplay" in el.dataset,
        speed: parseFloat(el.dataset.speed || "1"),
      });
      if (el.dataset.poster) ctl.seek(parseFloat(el.dataset.poster));
      el._sloshcast = ctl; // for whoever orchestrates several players
      el.dispatchEvent(new CustomEvent("sloshcast", { bubbles: true }));
    });
}
