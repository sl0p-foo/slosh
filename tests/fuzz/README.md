# Fuzz harness

libFuzzer + ASan + UBSan targets for every self-contained parser surface in
slosh.  Each target compiles only its own `.c` against its header — no vendor
libraries, no libghostty-vt linking — so the build is fast and the coverage
is tight.

## Targets

| target    | source            | attack surface                         |
|-----------|-------------------|----------------------------------------|
| `kdl`     | `src/kdl.c`       | config and layout files                |
| `jsonval` | `src/jsonval.c`   | control-API JSON (MSG_CMD on the socket) |
| `expr`    | `src/expr.c`      | shader expression compiler + evaluator |
| `proto`   | `src/proto.c`     | client↔server wire framing             |
| `osc5577` | `src/osc5577.c`   | pane status-bar escape sequences       |
| `input`   | `src/input.c`     | outer-terminal keyboard/mouse/paste    |

## Quick start

```
cd tests/fuzz
make                   # build libFuzzer binaries
make run-kdl TIME=30   # fuzz one target for 30 seconds
make run-all TIME=15   # fuzz all targets, 15 seconds each
make replay            # build file-replay binaries (no libFuzzer)
make corpus            # replay seed corpora (CI gate, no fuzzing)
```

Crashes land in `findings/<target>/`; grown corpora stay in `corpus/<target>/`.

## Reproducing a crash

```
./replay_kdl findings/kdl/crash-abc123   # under ASan, no fuzzer runtime
gdb --args ./replay_kdl findings/kdl/crash-abc123
```

## Adding a target

1. Write `fuzz_foo.c` with `LLVMFuzzerTestOneInput`.
2. Add `foo` to `TARGETS` in the Makefile.
3. Drop seed inputs in `corpus/foo/`.

The Makefile convention is that `fuzz_foo.c` + `src/foo.c` compile together
with no other objects.  If a new target needs more, add an explicit rule.
