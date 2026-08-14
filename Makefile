# sl0ppty — see DESIGN.md
#
# zig is the C compiler: it is already required to build libghostty-vt, and it
# gives us static musl linking and cross-compilation for free (D12).

ZIG      ?= $(HOME)/zig-0.16.0/zig
CC       := $(ZIG) cc
VT       := vendor/libghostty-vt
VT_OUT   := $(VT)/zig-out
VT_LIB   := $(VT_OUT)/lib/libghostty-vt.a
VT_INC   := $(VT_OUT)/include

# -MMD -MP emits a .d per object listing the headers it included, so editing a
# header rebuilds what actually uses it. Without this a header edit rebuilt
# nothing and the only safe move was `make clean`, which is how a 0.1s build
# turns into a habit of throwing the whole thing away.
CFLAGS   := -std=c23 -O1 -g -Wall -Wextra -Wno-unused-parameter -MMD -MP \
            -I$(VT_INC) -Isrc
LDFLAGS  :=

SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:src/%.c=build/%.o)
DEPS     := $(OBJ:.o=.d)
BIN      := build/sl0ppty

# One stamp per python test file. The stamp depends on the file, the harness and
# the binary, so an untouched test whose binary has not changed does not run
# again — and `make -j` runs the rest at once, because they are separate
# processes that share nothing (script mode opens no socket).
# test_session.py drives a real pty and belongs to test-live; it is only named
# test_* by history.
PY_TESTS  := $(filter-out test_session.py,$(notdir $(wildcard tests/test_*.py)))
PY_STAMPS := $(PY_TESTS:%.py=build/.pass-%)
JOBS      ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all clean vendor run test retest test-live test-all smoke help
.DEFAULT_GOAL := help

help: ## show this
	@grep -hE '^[a-z-]+:.*##' $(MAKEFILE_LIST) | sed 's/:.*##/\t/' | expand -t20

all: $(BIN) ## build sl0ppty

$(BIN): $(OBJ) $(VT_LIB)
	$(CC) $(OBJ) $(VT_LIB) -o $@ $(LDFLAGS)

build/%.o: src/%.c $(VT_LIB) | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	@mkdir -p build

$(VT_LIB):
	@$(MAKE) vendor

vendor: ## build the vendored libghostty-vt (needs zig 0.16)
	cd $(VT) && PATH="$(dir $(ZIG)):$$PATH" zig build -Demit-lib-vt -Doptimize=ReleaseFast

TEST_BIN := build/input_test

$(TEST_BIN): tests/input_test.c src/input.c $(VT_LIB) | build
	$(CC) $(CFLAGS) tests/input_test.c src/input.c $(VT_LIB) -o $@

KDL_TEST := build/kdl_test

$(KDL_TEST): tests/kdl_test.c src/kdl.c | build
	$(CC) $(CFLAGS) tests/kdl_test.c src/kdl.c -o $@

SHADER_TEST := build/shader_test

$(SHADER_TEST): tests/shader_test.c src/shader.c src/screen.c src/json.c $(VT_LIB) | build
	$(CC) $(CFLAGS) tests/shader_test.c src/shader.c src/screen.c src/json.c $(VT_LIB) -o $@

# Shader plugins, for test_shader_plugin.py: a good one, one that announces an
# ABI we do not speak, and the example we ship in contrib -- which is built
# here so that a broken example is a failing test rather than a bug report.
PLUGIN_CFLAGS := -std=c23 -O1 -fPIC -shared -Isrc

build/testshader.so: tests/shader_plugin_test.c src/shader_abi.h | build
	$(CC) $(PLUGIN_CFLAGS) $< -o $@

build/badshader.so: tests/shader_plugin_test.c src/shader_abi.h | build
	$(CC) $(PLUGIN_CFLAGS) -DBAD_ABI $< -o $@

build/exampleshader.so: contrib/shader-plugin/example.c src/shader_abi.h | build
	$(CC) $(PLUGIN_CFLAGS) $< -o $@

build/.pass-test_shader_plugin: build/testshader.so build/badshader.so \
                                build/exampleshader.so

build/.pass-%: tests/%.py tests/harness.py $(BIN) | build
	@cd tests && timeout 300 python3 $(notdir $<) > ../build/.log-$* 2>&1 \
	  || { echo "FAIL $(notdir $<)"; tail -25 ../build/.log-$*; exit 1; }
	@printf '  ok   %-24s %s\n' "$(notdir $<)" "$$(grep -c '^ok' build/.log-$* 2>/dev/null || echo ?) checks"
	@touch $@

test: $(BIN) $(TEST_BIN) $(KDL_TEST) $(SHADER_TEST) ## unit + headless checks (fast)
	@./$(TEST_BIN) >/dev/null && ./$(KDL_TEST) >/dev/null && ./$(SHADER_TEST) >/dev/null \
	  && printf '  ok   %-24s %s\n' "C unit tests" "3 binaries"
	@$(MAKE) --no-print-directory -j$(JOBS) $(PY_STAMPS)
	@printf '\nall green\n'

retest: ## force every python test to run again
	@rm -f build/.pass-*
	@$(MAKE) --no-print-directory test

test-live: $(BIN) ## the checks that need a real tty (slow)
	python3 tests/live_m0.py
	@echo
	python3 tests/live_input.py
	@echo
	python3 tests/test_session.py
	@echo
	python3 tests/live_reload.py

test-all: test test-live ## everything

smoke: $(BIN) ## compose a screen headlessly and print it
	./$(BIN) --headless --cols 40 --rows 12 -- /bin/sh -c 'printf "hello \033[1;32msl0ppty\033[0m\n"; echo "wide: 日本語"'

run: $(BIN) ## build and run
	./$(BIN)

clean: ## remove our build output (not the vendor lib)
	rm -rf build

-include $(DEPS)
