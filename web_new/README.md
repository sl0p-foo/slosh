# web_new — the slosh website

One front page, plus the two things the repository already builds, assembled
into a single static site:

| path | what | comes from |
|---|---|---|
| `/` | the front page: what it is, the demo video, install | `index.html` + `site.css`, hand-written, no build step |
| `/docs/` | the documentation | `contrib/gen-docs`, the same pages `make docs` renders |
| `/demo/` | the in-browser machine | `contrib/webdemo`, on its own page so the front page stays light |
| `/install.sh` | what `curl \| sudo sh` fetches | `install.sh` here |

## Build

```sh
web_new/build-site              # everything -> build/www
NO_DEMO=1 web_new/build-site    # without the demo (it cross-compiles a guest)
contrib/webdemo/serve --dir build/www
```

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

- The demo video is a placeholder (`#video` in `index.html`; the comment there
  says exactly what to replace). Drop in `demo.mp4` + `demo-poster.png` when
  rendered.
- `install.sh` clones `https://slosh.foo/src` (override: `SLOSH_REPO=`). Pin
  the real public repo URL once it exists — same for the `src` links in
  `index.html`.
