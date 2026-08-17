#!/usr/bin/env python3
"""`make docs`: docs/ rendered as a static site by contrib/gen-docs.

Two kinds of check, because the generator has two jobs.

The site: every page in docs/nav becomes a page, every page carries the whole
sidebar, prev/next chains the order, and every internal link lands on a file
that exists. A docs site whose links rot is worse than a directory of Markdown,
which at least a forge will render.

The Markdown subset: what it understands, it has to understand *exactly* --
notably that markup inside a code span is not markup, because half of these docs
are shader expressions full of asterisks.
"""

import html
import os
import re
import subprocess
import sys
import tempfile

from harness import check, report

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GEN = os.path.join(ROOT, "contrib", "gen-docs")
DOCS = os.path.join(ROOT, "docs")


def run(outdir, docs=None, expect_ok=True):
    env = dict(os.environ)
    if docs:
        env["SL0PPTY_DOCS"] = docs
    r = subprocess.run(
        [sys.executable, GEN, outdir], capture_output=True, text=True, env=env
    )
    if expect_ok and r.returncode != 0:
        check("gen-docs succeeds", False, r.stderr.strip() or r.stdout.strip())
    return r


def nav_pages():
    pages, sections = [], []
    for raw in open(os.path.join(DOCS, "nav")):
        line = raw.rstrip()
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        (pages if line[0].isspace() else sections).append(line.strip())
    return sections, pages


def test_it_renders_every_page_in_the_nav():
    out = tempfile.mkdtemp()
    run(out)
    sections, pages = nav_pages()
    check(
        "docs/nav has sections and pages",
        sections and len(pages) >= 8,
        f"{sections} / {pages}",
    )

    for md in pages:
        f = os.path.join(out, md[:-3] + ".html")
        check(f"{md} was rendered", os.path.exists(f), f)
    check("the stylesheet came with it", os.path.exists(os.path.join(out, "docs.css")))

    body = open(os.path.join(out, "keys.html")).read()
    for md in pages:
        check(
            f"keys.html links to {md} in its sidebar",
            'href="%s"' % (md[:-3] + ".html") in body,
            md,
        )
    check(
        "the current page is marked in the sidebar",
        'class="here" href="keys.html"' in body,
        "no here marker",
    )
    check("its section is the crumb", 'class="crumb">Using it' in body, body[:400])


def test_every_internal_link_lands_somewhere():
    out = tempfile.mkdtemp()
    run(out)
    broken = []
    for f in sorted(os.listdir(out)):
        if not f.endswith(".html"):
            continue
        for href in re.findall(r'href="([^"]+)"', open(os.path.join(out, f)).read()):
            if href.startswith(("http://", "https://", "#", "mailto:")):
                continue
            target = href.split("#")[0]
            if target and not os.path.exists(os.path.join(out, target)):
                broken.append(f"{f} -> {href}")
    check("no internal link is broken", not broken, "; ".join(broken[:6]))


def test_prev_and_next_chain_the_nav_order():
    out = tempfile.mkdtemp()
    run(out)
    _, pages = nav_pages()
    first = open(os.path.join(out, pages[0][:-3] + ".html")).read()
    middle = open(os.path.join(out, pages[1][:-3] + ".html")).read()
    last = open(os.path.join(out, pages[-1][:-3] + ".html")).read()
    check(
        "the first page has a next and no prev",
        'class="next"' in first and 'class="prev"' not in first,
        "first",
    )
    check(
        "a middle page has both",
        'class="next"' in middle and 'class="prev"' in middle,
        "middle",
    )
    check(
        "the last page has a prev and no next",
        'class="prev"' in last and 'class="next"' not in last,
        "last",
    )


def test_a_page_missing_from_the_nav_is_refused():
    """Otherwise a page written today is a page nobody can reach, and the only
    sign of it is its absence."""
    docs = tempfile.mkdtemp()
    with open(os.path.join(docs, "nav"), "w") as f:
        f.write("Start\n  index.md\n")
    with open(os.path.join(docs, "index.md"), "w") as f:
        f.write("# Start\n\nhello\n")
    with open(os.path.join(docs, "orphan.md"), "w") as f:
        f.write("# Orphan\n\nnobody links here\n")
    r = run(tempfile.mkdtemp(), docs=docs, expect_ok=False)
    check("an orphan page fails the build", r.returncode != 0, r.stdout)
    check(
        "and it says which one",
        "orphan.md" in (r.stderr + r.stdout),
        r.stderr + r.stdout,
    )


def test_a_nav_entry_with_no_file_is_refused():
    docs = tempfile.mkdtemp()
    with open(os.path.join(docs, "nav"), "w") as f:
        f.write("Start\n  index.md\n  ghost.md\n")
    with open(os.path.join(docs, "index.md"), "w") as f:
        f.write("# Start\n\nhello\n")
    r = run(tempfile.mkdtemp(), docs=docs, expect_ok=False)
    check("a nav entry with no page fails the build", r.returncode != 0, r.stdout)
    check(
        "and it says which one",
        "ghost.md" in (r.stderr + r.stdout),
        r.stderr + r.stdout,
    )


def render_fixture(md):
    """One page through the real generator; returns its HTML."""
    docs = tempfile.mkdtemp()
    with open(os.path.join(docs, "nav"), "w") as f:
        f.write("Start\n  index.md\n")
    with open(os.path.join(docs, "index.md"), "w") as f:
        f.write(md)
    out = tempfile.mkdtemp()
    run(out, docs=docs)
    return open(os.path.join(out, "index.html")).read()


def test_the_markdown_subset():
    got = render_fixture("""# Title

A paragraph with `code`, **bold**, *italic* and a [link](keys.md#defaults).

## A heading

- one
- two
  - nested

| a | b |
|---|---|
| `x` | y |

```kdl
shaders { dim amount="(y % 2) * 40" }
```

```
╭─────╮
╰─────╯
```

> a quote

---
""")
    cases = {
        "the h1 is the title": "<title>Title</title>" in got,
        "headings get an id and an anchor": '<h2 id="a-heading">' in got
        and 'href="#a-heading"' in got,
        "inline code": "<code>code</code>" in got,
        "bold": "<strong>bold</strong>" in got,
        "italic": "<em>italic</em>" in got,
        "a link to another page points at its html": 'href="keys.html#defaults"' in got,
        "lists nest": "<ul>" in got and "<li>nested</li>" in got,
        "tables": "<table>" in got and "<th>a</th>" in got,
        "a fenced block keeps its language": 'class="lang-kdl"' in got,
        "and its contents verbatim": html.escape('dim amount="(y % 2) * 40"') in got,
        "box drawing is marked as art, not code": 'class="art"' in got,
        "blockquotes": "<blockquote>" in got,
        "rules": "<hr>" in got,
    }
    for name, ok in cases.items():
        check(name, ok, got[:600])


def test_markup_inside_a_code_span_is_not_markup():
    """Half of these docs are shader expressions. A rule that formats the
    asterisks inside them is a rule that mangles the documentation."""
    got = render_fixture('# T\n\n`amount="(x < 10) * 200 ** 2"` and **bold**\n')
    check(
        "asterisks in a code span survive",
        html.escape("(x < 10) * 200 ** 2") in got,
        got[:400],
    )
    check("bold outside it still works", "<strong>bold</strong>" in got, got[:400])
    check(
        "and the span is not a strong tag",
        "<strong>" in got and got.count("<strong>") == 1,
        got[:400],
    )


def test_html_in_the_source_is_escaped():
    got = render_fixture("# T\n\nnot <script>alert(1)</script> markup\n")
    check("a tag in the source is text", "&lt;script&gt;" in got, got[:400])
    check("and not a tag", "<script>" not in got, got[:400])


if __name__ == "__main__":
    test_it_renders_every_page_in_the_nav()
    test_every_internal_link_lands_somewhere()
    test_prev_and_next_chain_the_nav_order()
    test_a_page_missing_from_the_nav_is_refused()
    test_a_nav_entry_with_no_file_is_refused()
    test_the_markdown_subset()
    test_markup_inside_a_code_span_is_not_markup()
    test_html_in_the_source_is_escaped()
    sys.exit(report())
