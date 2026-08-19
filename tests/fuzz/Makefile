# Standalone fuzzing harness for slosh's parser surfaces.
#
# Deliberately separate from the main build: the main build uses zig cc for
# static musl linking, but libFuzzer wants clang's own runtime, and the six
# parsers under test include nothing but their own headers -- so this Makefile
# needs no vendor step, no libghostty-vt, nothing but clang and ../../src.
#
#   make            build every fuzz_* binary (libFuzzer + ASan + UBSan)
#   make replay     build every replay_* binary (same target, file-replay main)
#   make corpus     check the seed corpora replay cleanly (a fast CI gate)
#   make run-kdl    fuzz one target (TIME=60 seconds, default 30)
#   make run-all    fuzz every target for TIME seconds each, sequentially
#
# Findings land in findings/<target>/ and grown corpora stay in corpus/<target>/.

CLANG ?= clang
ROOT  := ../..

TARGETS := kdl jsonval expr proto osc5577 input
TIME    ?= 30

# src/input.c includes <ghostty/vt.h> for key/mod constants only (it calls
# nothing), so the vendored header is enough and no library is linked. It
# exists once the main build's `make vendor` has run.
VT_INC := $(ROOT)/vendor/libghostty-vt/zig-out/include

CFLAGS := -std=c23 -O1 -g -Wall -Wextra -Wno-unused-parameter \
          -I$(ROOT)/include -I$(VT_INC) \
          -fsanitize=address,undefined -fno-sanitize-recover=all

FUZZ_BINS   := $(TARGETS:%=fuzz_%)
REPLAY_BINS := $(TARGETS:%=replay_%)

all: $(FUZZ_BINS) ## build libFuzzer binaries

replay: $(REPLAY_BINS) ## build file-replay binaries (no libFuzzer runtime)

# Each target is fuzz_<name>.c + src/<name>.c, nothing else. If that ever
# stops being true for a new target, list its extra objects explicitly.
fuzz_%: fuzz_%.c $(ROOT)/src/%.c
	$(CLANG) $(CFLAGS) -fsanitize=fuzzer $^ -o $@

replay_%: fuzz_%.c $(ROOT)/src/%.c standalone_main.c
	$(CLANG) $(CFLAGS) $^ -o $@

run-%: fuzz_%
	@mkdir -p findings/$* corpus/$*
	./fuzz_$* -max_total_time=$(TIME) -max_len=65536 \
	    -artifact_prefix=findings/$*/ corpus/$*

run-all: $(FUZZ_BINS) ## fuzz every target for TIME seconds each
	@for t in $(TARGETS); do $(MAKE) --no-print-directory run-$$t || exit 1; done

corpus: $(REPLAY_BINS) ## replay all seed corpora (CI-friendly, no fuzzing)
	@for t in $(TARGETS); do \
	    files=$$(find corpus/$$t -type f 2>/dev/null); \
	    if [ -n "$$files" ]; then ./replay_$$t $$files || exit 1; fi; \
	done
	@echo "corpus replay: all clean"

clean:
	rm -f $(FUZZ_BINS) $(REPLAY_BINS)

.PHONY: all replay run-all corpus clean
