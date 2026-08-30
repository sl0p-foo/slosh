// The page half of the demo: build a terminal, hand its size to a riscv64
// TinyEMU compiled to wasm, and shovel bytes between the two.
//
// Everything here talks to a *prebuilt* emulator -- Fabrice Bellard's
// riscvemu64-wasm.js, MIT, https://bellard.org/tinyemu/. That blob was
// compiled against TinyEMU's js/lib.js, whose emscripten glue reaches straight
// for globals: `term`, `update_downloading`, `graphic_display`, `net_state`,
// `Module`. So those are plain top-level `var`s and functions here,
// deliberately -- no module scope, no bundler, nothing that would hide them
// from a blob we do not compile. Rename one and the VM dies at boot with a
// bare ReferenceError from inside minified code.

/* `term` is a two-method shim, not the xterm object: lib.js only ever calls
   write() and getSize(), and keeping them apart means the emulator cannot
   accidentally depend on the rest of xterm's API. */
var term = null;
var graphic_display = null; /* text-only, but _fb_refresh reads it unguarded */
var net_state = null;       /* no networking: no socket to relay packets to */
var Module = {};            /* emscripten adopts an existing global Module */
var console_write1 = null;  /* set in preRun: one byte -> the guest console */

/* Root filesystem blocks arrive over HTTP, lazily, 256 KiB at a time, so this
   fires during ordinary use and not just at boot. Worth showing: a pause that
   comes with a visible "fetching" reads as a download, and the same pause
   without one reads as a hang. Hidden on a delay because bursts of block
   fetches would otherwise strobe. */
var downloading_timer = null;

function update_downloading(flag) {
    var el = document.getElementById("fetching");
    if (!el)
        return;
    if (flag) {
        if (downloading_timer !== null) {
            clearTimeout(downloading_timer);
            downloading_timer = null;
        }
        el.classList.add("on");
    } else if (downloading_timer === null) {
        downloading_timer = setTimeout(function () {
            downloading_timer = null;
            el.classList.remove("on");
        }, 500);
    }
}

(function () {
    "use strict";

    /* Parsed the way jslinux.js does it, so a URL that works there works here:
       ?mem= (MB), ?cols=, ?rows=, ?cfg= (another VM config), ?cmdline= (extra
       kernel arguments, appended to the config's own). */
    function get_params() {
        var href = window.location.href;
        var p = href.indexOf("?");
        var params = {};
        if (p < 0)
            return params;
        href.substr(p + 1).split("&").forEach(function (pair) {
            var kv = pair.split("=");
            params[decodeURIComponent(kv[0])] = decodeURIComponent(kv[1] || "");
        });
        return params;
    }

    /* The emulator fetches the config with an XHR of its own and resolves
       drive0's blocks relative to it, so it has to be absolute. */
    function get_absolute_url(fname) {
        if (fname.indexOf(":") >= 0)
            return fname;
        var path = window.location.pathname;
        var p = path.lastIndexOf("/");
        if (p < 0)
            return fname;
        return window.location.origin + path.slice(0, p + 1) + fname;
    }

    function clamp(v, lo, hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    var params = get_params();
    var cfg_url = get_absolute_url(params["cfg"] || "slosh.cfg");
    var mem_size = (params["mem"] | 0) || 128;
    var cmdline = params["cmdline"] || "";

    /* ?embed=1: the same page inside the front page's iframe. The chrome
       around the stage belongs to the standalone page; embedded, the stage
       is the whole story (demo.css hides the rest and fills the frame). */
    if (params["embed"])
        document.body.classList.add("embed");

    var stage = document.getElementById("stage");
    var wrap = document.getElementById("term_wrap");
    var status_el = document.getElementById("status");

    /* xterm.js rather than Bellard's term.js, which is the whole point of the
       exercise: term.js has no truecolor (38;2) and no wide-character
       handling, and slosh paints 24-bit colour shaders and lays out CJK. */
    var xt = new Terminal({
        cols: 80,
        rows: 24,
        cursorBlink: true,
        convertEol: false,       /* the guest sends proper CRLF; do not "help" */
        scrollback: 5000,
        fontSize: 14,
        fontFamily: '"JetBrains Mono", "Fira Code", Menlo, Consolas, "DejaVu Sans Mono", monospace',
        theme: {
            /* contrib/themes/sl0p.kdl: hot pink on near-black */
            background: "#0b0b0e",
            foreground: "#e8e8ea",
            cursor: "#ff5fd7",
            cursorAccent: "#0b0b0e",
            selectionBackground: "rgba(255, 95, 215, 0.30)",
            black: "#141418",
            red: "#ff5f5f",
            green: "#5fd7a7",
            yellow: "#ffd75f",
            blue: "#5fafff",
            magenta: "#ff5fd7",
            cyan: "#5fd7d7",
            white: "#e8e8ea",
            brightBlack: "#45454a",
            brightRed: "#ff8787",
            brightGreen: "#87ffd7",
            brightYellow: "#ffe787",
            brightBlue: "#87cfff",
            brightMagenta: "#ff87e7",
            brightCyan: "#87e7e7",
            brightWhite: "#ffffff"
        }
    });
    xt.open(wrap);

    /* Cell metrics come off the rendered grid rather than out of a table: the
       font that actually resolved is the only one whose cell size is true. */
    function measure_cell() {
        var screen = wrap.querySelector(".xterm-screen");
        if (screen) {
            var r = screen.getBoundingClientRect();
            if (r.width > 0 && r.height > 0)
                return { w: r.width / xt.cols, h: r.height / xt.rows };
        }
        return { w: 8.5, h: 17 };  /* headless or display:none; keep going */
    }

    /* The room a scaled terminal has: the stage's content box, padding taken
       off, since the wrapper sits inside that padding. */
    function room() {
        var cs = window.getComputedStyle(stage);
        var w = stage.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
        var h = stage.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom);
        return { w: w > 0 ? w : 1024, h: h > 0 ? h : 640 };
    }

    /* Geometry is chosen once, before vm_start, and then never again: jsemu.c
       sets console_resize_pending only in console_init(), and exports no
       resize entry point, so the guest hears the size exactly once at boot.
       Nothing this page can do afterwards will reach it -- hence the font
       refit below instead of a reflow. The caps exist only to keep a huge
       monitor from asking the emulated CPU to repaint a stadium screen. */
    var cell = measure_cell();
    var box = room();
    var cols = clamp(Math.floor(box.w / cell.w), 80, 220);
    var rows = clamp(Math.floor(box.h / cell.h), 24, 60);
    if (params["cols"])
        cols = clamp(params["cols"] | 0, 20, 400);
    if (params["rows"])
        rows = clamp(params["rows"] | 0, 8, 200);
    xt.resize(cols, rows);

    var geom_el = document.getElementById("geometry");
    if (geom_el)
        geom_el.textContent = cols + "x" + rows;

    /* Fill the room by choosing a font size, never by a scale() transform.
       xterm turns a mouse event into a cell with getBoundingClientRect() and
       its own unscaled cell metrics; a transform it cannot see lands every
       click on the wrong cell, off by the scale factor. Desktop kept the
       factor near 1 (cols and rows were derived from this very room), so it
       passed for working; a phone is forced onto the 80-column floor, the
       factor lands near a half, and a tap selects a pane the finger never
       touched. Refitting the font moves the same pixels honestly: the grid
       stays what the guest was told, the drawn rect is the measured rect,
       and the leftover is centred with translate -- which moves rect and
       clicks together, and is the part of a transform xterm does see. */
    function refit() {
        var b = room();
        /* Cell metrics are not linear in font size (hinting rounds each step
           its own way), so walk until fixed -- two rounds in practice. */
        for (var i = 0; i < 4; i++) {
            var c = measure_cell();
            var fit = Math.min(b.w / (c.w * cols), b.h / (c.h * rows));
            var next = clamp(Math.floor(xt.options.fontSize * fit), 6, 32);
            if (next === xt.options.fontSize)
                break;
            xt.options.fontSize = next;
        }
        var r = wrap.getBoundingClientRect();
        var dx = Math.max(0, (b.w - r.width) / 2);
        wrap.style.transform = "translate(" + dx.toFixed(2) + "px, 0px)";
    }

    refit();
    window.addEventListener("resize", refit);
    /* A webfont that resolves after boot changes the cell metrics under the
       fit; refit again when the fonts settle. The grid cannot move, only the
       glyphs. */
    if (document.fonts && document.fonts.ready)
        document.fonts.ready.then(refit);

    /* Output: lib.js builds the string with String.fromCharCode over the
       guest's bytes, so each char code is one byte and the string is latin1,
       not decoded text. Widen it back to bytes and let xterm do the UTF-8 --
       that is what makes a wide glyph occupy two cells instead of arriving as
       two mojibake ones. */
    var booted = false;

    term = {
        write: function (str) {
            if (!booted) {
                booted = true;
                if (status_el)
                    status_el.hidden = true;
            }
            xt.write(Uint8Array.from(str, function (c) {
                return c.charCodeAt(0) & 0xff;
            }));
        },
        getSize: function () {
            return [cols, rows];
        }
    };

    /* Input: the reverse, and the place jslinux.js has a bug worth not
       copying. It sends charCodeAt(i) per JS char, which truncates anything
       past U+00FF and splits everything else wrong; encode to UTF-8 and send
       every byte. */
    var encoder = new TextEncoder();

    xt.onData(function (s) {
        if (!console_write1)
            return;
        var bytes = encoder.encode(s);
        for (var i = 0; i < bytes.length; i++)
            console_write1(bytes[i]);
    });

    stage.addEventListener("mousedown", function () { xt.focus(); });
    xt.focus();

    /* The cheat-sheet lives in a native <dialog>: showModal() gives Esc and a
       backdrop for free, and while it is open the terminal is not focused, so
       nothing typed at the sheet leaks into the guest. Clicking the backdrop
       is a click whose target is the dialog element itself. */
    var keys_dlg = document.getElementById("keys");
    var keys_btn = document.getElementById("keys_btn");
    if (keys_dlg && keys_btn && keys_dlg.showModal) {
        keys_btn.addEventListener("click", function () { keys_dlg.showModal(); });
        keys_dlg.addEventListener("click", function (ev) {
            if (ev.target === keys_dlg)
                keys_dlg.close();
        });
        keys_dlg.addEventListener("close", function () { xt.focus(); });
    }

    /* preRun runs after the wasm instance exists and before main(), which is
       the only window where cwrap works and the VM has not started. */
    function start() {
        console_write1 = Module.cwrap("console_queue_char", null, ["number"]);
        /* width/height 0 says text console; the 0 after them is "no network". */
        Module.ccall("vm_start", null,
                     ["string", "number", "string", "string", "number", "number", "number", "string"],
                     [cfg_url, mem_size, cmdline, "", 0, 0, 0, ""]);
    }

    Module.preRun = start;

    /* Loaded last and by hand, so every global above is in place first. */
    var script = document.createElement("script");
    script.src = "riscvemu64-wasm.js";
    script.onerror = function () {
        if (status_el)
            status_el.textContent = "could not load riscvemu64-wasm.js -- run `make all`";
    };
    document.getElementsByTagName("head")[0].appendChild(script);
})();
