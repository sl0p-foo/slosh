# SPEED — sl0ppty

All numbers measured on this box, 2026-08-14. Re-measure only if something
structural changes.

| what | command | time |
|---|---|---|
| build (incremental) | `make all -j8` | ~0.1s |
| build (clean) | `make clean && make all -j8` | ~0.15s (zig caches objects) |
| C unit tests | `./build/{input,kdl,shader}_test` | ~0.00s |
| one python test | `cd tests && python3 test_shaders.py` | 0.6–7s |
| **fast suite** | `make test` | **~5.5s** (parallel, `-j$(nproc)`) |
| fast suite, nothing changed | `make test` | ~0.02s (per-file stamps) |
| fast suite, one test touched | `make test` | ~1.1s |
| force re-run everything | `make retest` | ~5.5s |
| live suite (real ptys) | `make test-live` | ~5.7s |

## Notes

- `make test` runs each `tests/test_*.py` as its own make target, so they run
  in parallel and an unchanged test whose binary has not changed is skipped.
  Stamps live in `build/.pass-*`, logs in `build/.log-*`.
- Touching a **header** does rebuild the objects that include it (`-MMD -MP`).
  There is no reason to `make clean`; a clean build is 0.15s anyway.
- `test_session.py` is a *live* test (real pty) and is excluded from `make
  test` on purpose — it runs in `make test-live`.

## Writing fast tests here

`settle N` is the only thing that reads a pane's pty, but it is a **duration**,
i.e. a guess. `send` and `snapshot` are already synchronous with the app, so:

- asserting on **our own chrome** (frames, guides, shaders, hints) after a
  mouse or key event needs **no settle at all**;
- waiting for a **program's output** should use `s.until_text("...")` or
  `s.until(pred)`, which pump in 10ms steps and return the instant the
  condition holds;
- anything gated on a **real clock** (hover dwell, toast expiry) is a genuine
  wait — so configure the clock down (`hover_delay_ms 20`, `toast_ms 150`)
  rather than sleeping for the default.

Getting this wrong is what made the suite 80s. It is 5.5s now.

## Not a trap

`speedscan` flags `make test` as spinning up docker/compose. It does not —
that is a false positive from the word `compose` (as in *compositing a
screen*) appearing all over this codebase.
