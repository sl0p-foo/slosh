# web — the slosh website

Everything the website is made of lives here (plus the docs generator, which
`make docs` shares). One front page, the documentation, and the in-browser
demo, assembled into a single static site:

| path | what | comes from |
|---|---|---|
| `/` | the front page: what it is, the demo video, install | `index.html` + `site.css`, hand-written, no build step |
| `/docs/` | the documentation | `contrib/gen-docs`, the same pages `make docs` renders |
| `/demo/` | the in-browser machine | `demo/`, on its own page so the front page stays light |
| `/install.sh` | what `curl \| sudo sh` fetches | `install.sh` here |
| `/recordings/` | scripted feature playbacks the front page embeds | `recordings/`, native format + player — see `recordings/PLAN.md` |

## Build

```sh
make www                    # for now: the coming-soon page -> build/www
FULL=1 web/build-site       # the real site (front page + docs + demo)
NO_DEMO=1 FULL=1 web/build-site  # ...without the demo (it cross-compiles a guest)
web/demo/serve --dir build/www
```

Until the site is finished, the default build is `coming-soon.html` alone
(one self-contained file, same session-drawn look). Deploying is copying
`build/www` to `/opt/slosh` on the box; drop the gate in `build-site` when
the real thing ships.

The output is only static files. Any host that serves a directory works, with
the one condition the webdemo README already states: `.wasm` must be served as
`application/wasm`.

## Decisions

- **The page is drawn like a session.** Tab strip as the nav, sections as
  panes with the title set into the frame, a status line at the bottom. The
  palette is `contrib/themes/sl0p.kdl`. This is what keeps it from looking
  like every generated landing page: it looks like slosh, because it copies
  slosh rather than a template.
- **The demo lives on its own page.** Loading it costs ~11 MiB and a machine
  boot; the front page costs two files and no JavaScript beyond the copy
  buttons.
- **No framework, no fonts fetched, no tracker.** Two files for the front
  page. The system monospace stack everywhere the terminal is being imitated.

## TODO

- Every feature in the tour has its recording now; what remains on the
  recordings side (a detach demo, storyline polish, static no-JS posters) is
  in `recordings/PLAN.md`.
- `install.sh` downloads the prebuilt binary from `https://slosh.foo/dist/`
  (override: `SLOSH_DIST=`), picking the newest version listed in its
  `SHA256SUMS`. Once the GitHub repo is public, the front page and the
  script can point at GitHub releases instead — same for the `src` links in
  `index.html`.
- The windows zips (`make release-windows`) are not uploaded to `/dist/` yet;
  the windows install tab points there, so upload them with the next release.
