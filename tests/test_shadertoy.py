#!/usr/bin/env python3
"""contrib/shadertoy.html evaluates the same expressions we do.

The toy reimplements src/expr.c in JavaScript so it can run in a browser with
no build and no server. That duplication is the whole hazard: a preview that
disagrees with the real thing is worse than no preview, and it would disagree
quietly, in the third digit of a colour.

So both implementations are handed the same expressions at the same points and
have to produce the same numbers. The JS is lifted straight out of the HTML —
if someone edits the page, this tests what the page actually contains.

Skipped, loudly, when node is missing: it is a checker for a contrib toy, not
a reason nobody can run the suite.
"""
import json
import os
import random
import shutil
import subprocess
import sys

from harness import check, report

HERE = os.path.dirname(os.path.abspath(__file__))
HTML = os.path.join(HERE, "..", "contrib", "shadertoy.html")
EVAL = os.path.join(HERE, "..", "build", "expr_eval")
SCRIPT = open(HTML).read().split("<script>")[1].split("</script>")[0]

# Environments to evaluate at: corners, the middle, and a moved cursor.
ENVS = [
    dict(x=0,  y=0,  cols=80, rows=24, curx=0,  cury=0,  cursor=1, focused=1, t=0,     since=0),
    dict(x=13, y=7,  cols=80, rows=24, curx=40, cury=12, cursor=1, focused=1, t=1234,  since=120),
    dict(x=79, y=23, cols=80, rows=24, curx=3,  cury=21, cursor=0, focused=0, t=99999, since=8000),
    dict(x=40, y=12, cols=100, rows=40, curx=99, cury=39, cursor=1, focused=1, t=7,    since=250),
]

def page_presets():
    """Every preset the page ships, lifted out of the page.

    Read rather than listed, so a preset added later is checked without anyone
    remembering to add it here -- which nobody would."""
    prefix = SCRIPT[:SCRIPT.index("/* ---- the chain UI")]
    out = subprocess.run(["node", "-e", prefix + "\nconsole.log(JSON.stringify(PRESETS));"],
                         capture_output=True, text=True)
    groups = json.loads(out.stdout)
    return [(f"{group}/{name}", i, p["amount"])
            for group, entries in groups.items()
            for name, chain in entries.items()
            for i, p in enumerate(chain)]


EXPRS = [
    # the corners of the language
    "1 + 2 * 3",
    "(1 + 2) * 3",
    "10 - 3 - 2",
    "100 / 7",
    "7 % 3",
    "-5 + 1",
    "5 / 0",
    "5 % 0",
    "x / 0 + 3",
    "3 < 4",
    "4 <= 4",
    "3 == 3",
    "3 != 3",
    "x < curx",
    "1 && 0",
    "0 || x",
    "!0",
    "!x",
    "min(x, y)",
    "max(x, y)",
    "abs(y - rows)",
    "clamp(x * 9, 10, 200)",
    "dist(0, 0, 4, 0)",
    "dist(0, 0, 0, 2)",
    "dist(x, y, curx, cury)",
    "x ? 10 : 20",
    "cursor ? 255 : 0",
    "focused ? x * 2 : y * 3",
    "0 ? 1 / 0 : 7",
    "t % 255",
    "since",
    "(since < 250) * 255",             # a flash
    "abs(since / 8 % 510 - 255)",      # ...and a breathe
    "(x + y) % 7 * 30",
    "cols - x",
    "rows * 2 - y",
    # bitwise, where the two implementations agree by construction: int32, a
    # five-bit shift count, an arithmetic `>>`, and -- unlike C -- bits binding
    # tighter than comparison.
    "x & 7",
    "x | 8",
    "x ^ y",
    "~x",
    "~0",
    "x << 3",
    "x >> 1",
    "-8 >> 1",
    "-1 >> 28",
    "1 << 31",
    "1 << 33",
    "x << 40",
    "x >> 40",
    "0 - 5 & 3",
    "x & 7 == 5",
    "x ^ y | 1",
    "x | y & 1",
    "x ^ y ^ x",
    "(x ^ y) % 16 < 2",
    "(x & 1) ^ (y & 1)",
    "~x & 255",
    "t >> 6 & 15",
    "(x * 7 ^ y * 13) & 31",
    # trig: degrees in, -255..255 out, from a table both sides share
    "sin(0)", "sin(30)", "sin(45)", "sin(90)", "sin(180)", "sin(270)",
    "sin(359)", "sin(360)", "sin(450)", "sin(0 - 30)", "sin(0 - 400)",
    "cos(0)", "cos(90)", "cos(180)", "cos(270)", "cos(0 - 90)",
    "sin(x * 40)", "cos(y * 37)", "sin(t / 4)", "sin(t) + cos(t)",
    "128 + sin(t / 4) / 2",
    "abs(sin(x * 9 + t / 6))",
    "clamp(sin(t / 5) + 128, 0, 255)",
    "sin(sin(t / 3))",
]


def rand_exprs(n, seed=20260814):
    """A few generated ones too, so the checks are not only what I thought of."""
    rng = random.Random(seed)
    atoms = ["x", "y", "cols", "rows", "curx", "cury", "t", "since", "3", "7",
             "40", "255"]
    out = []
    for _ in range(n):
        a, b, c = (rng.choice(atoms) for _ in range(3))
        out.append(rng.choice([
            f"({a} {rng.choice(['+', '-', '*', '/', '%', '&', '|', '^'])} {b}) * 2",
            f"{a} {rng.choice(['<<', '>>'])} ({b} % 6)",
            f"({a} ^ {b}) & {c}",
            f"sin({a} * 3 + {b})",
            f"cos({a} - {b})",
            f"128 + sin({a} * {rng.choice(['2', '5', '11'])}) / 2",
            f"clamp({a} * {rng.choice([2, 3, 9])} - {b}, 0, 255)",
            f"({a} {rng.choice(['<', '>', '==', '<=', '!='])} {b}) * {c}",
            f"min({a}, {b}) + max({b}, {c})",
            f"{a} ? {b} : {c}",
            f"dist({a}, {b}, curx, cury) * 4",
            f"abs({a} - {b}) % ({c} + 1)",
        ]))
    return out


def c_values(pairs):
    """Every (env, expr) through the compiler sl0ppty actually uses."""
    lines = []
    for env, expr in pairs:
        vals = " ".join(str(env[k]) for k in
                        ("x", "y", "cols", "rows", "curx", "cury", "cursor",
                         "focused", "t", "since"))
        lines.append(f"{vals}\t{expr}")
    out = subprocess.run([EVAL], input="\n".join(lines) + "\n",
                         capture_output=True, text=True)
    return out.stdout.strip().split("\n")


JS_HARNESS = """
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");
// The page's script, run as-is except for the parts that need a document.
const script = html.split("<script>")[1].split("</script>")[0];
const body = script.slice(0, script.indexOf("/* ---- the fake pane"));
const compile = new Function(body + "; return compile;")();

const cases = JSON.parse(fs.readFileSync(process.argv[3], "utf8"));
const out = [];
for (const [env, expr] of cases) {
  try {
    const prog = compile(expr);
    let v = prog.fn(env);
    out.push(String(Math.trunc(v)));
  } catch (e) {
    out.push("error: " + e.message);
  }
}
console.log(out.join("\\n"));
"""


def main():
    if not shutil.which("node"):
        print("SKIP no node: contrib/shadertoy.html cannot be checked here")
        return 0
    if not os.path.exists(EVAL):
        check("the expression evaluator is built", False, EVAL)
        return report()

    presets = page_presets()
    check("the page ships presets to check", len(presets) >= 20, str(len(presets)))
    exprs = EXPRS + [e for _, _, e in presets] + rand_exprs(40)
    pairs = [(env, e) for e in exprs for env in ENVS]

    import tempfile
    cases = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump([[env, e] for env, e in pairs], cases)
    cases.close()
    harness = tempfile.NamedTemporaryFile("w", suffix=".js", delete=False)
    harness.write(JS_HARNESS)
    harness.close()

    js = subprocess.run(["node", harness.name, HTML, cases.name],
                        capture_output=True, text=True)
    os.unlink(cases.name)
    os.unlink(harness.name)
    if js.returncode != 0:
        check("the page's evaluator runs", False, js.stderr[-400:])
        return report()

    got = js.stdout.strip().split("\n")
    want = c_values(pairs)

    check("both sides answered every case",
          len(got) == len(pairs) and len(want) == len(pairs),
          f"js {len(got)}, c {len(want)}, cases {len(pairs)}")

    bad = [(e, env, w, g) for (env, e), w, g in zip(pairs, want, got) if w != g]
    check(f"the toy agrees with the compiler on all {len(pairs)} evaluations",
          not bad,
          "\n".join(f"    {e!r} at {env} -> C {w}, JS {g}" for e, env, w, g in bad[:8]))

    # A preset that computes 0 everywhere is a preset that does nothing, which
    # is a broken example rather than a subtle one: it looks like the shader
    # system is not working. Checked over a whole pane and four points in time,
    # cell by cell -- sampling every other row made three `y % 2` effects look
    # dead when they were fine.
    grid = [dict(x=x, y=y, cols=64, rows=24, curx=30, cury=12, cursor=1,
                 focused=1, t=t, since=t)
            for t in (0, 500, 1500, 4000) for y in range(24) for x in range(64)]
    flat = [(env, e) for _, _, e in presets for env in grid]
    vals = c_values(flat)
    per = len(grid)
    for idx, (label, i, expr) in enumerate(presets):
        window = vals[idx * per:(idx + 1) * per]
        nums = [int(v) for v in window if not v.startswith("error")]
        check(f"{label} pass {i} does something",
              nums and max(0, min(255, max(nums))) > 0,
              f"{expr!r} is 0 everywhere")

    # And that a refusal is a refusal on both sides, since a page that quietly
    # accepts what the config rejects teaches the wrong language.
    refused = ["nosuchvar + 1", "wobble(1, 2)", "(1 + 2", "1 +", "1 + 2 )", "1 ? 2"]
    rpairs = [(ENVS[0], e) for e in refused]
    cw = c_values(rpairs)
    cases = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump([[env, e] for env, e in rpairs], cases)
    cases.close()
    harness = tempfile.NamedTemporaryFile("w", suffix=".js", delete=False)
    harness.write(JS_HARNESS)
    harness.close()
    rjs = subprocess.run(["node", harness.name, HTML, cases.name],
                         capture_output=True, text=True).stdout.strip().split("\n")
    os.unlink(cases.name)
    os.unlink(harness.name)
    for expr, c, j in zip(refused, cw, rjs):
        check(f"both refuse {expr!r}",
              c.startswith("error") and j.startswith("error"), f"C {c!r}, JS {j!r}")

    return report()


sys.exit(main())
