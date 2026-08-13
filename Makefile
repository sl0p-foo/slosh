# sl0ptty — see DESIGN.md
#
# zig is the C compiler: it is already required to build libghostty-vt, and it
# gives us static musl linking and cross-compilation for free (D12).

ZIG      ?= $(HOME)/zig-0.16.0/zig
CC       := $(ZIG) cc
VT       := vendor/libghostty-vt
VT_OUT   := $(VT)/zig-out
VT_LIB   := $(VT_OUT)/lib/libghostty-vt.a
VT_INC   := $(VT_OUT)/include

CFLAGS   := -std=c23 -O1 -g -Wall -Wextra -Wno-unused-parameter \
            -I$(VT_INC) -Isrc
LDFLAGS  :=

SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:src/%.c=build/%.o)
BIN      := build/sl0ptty

.PHONY: all clean vendor run test test-live test-all smoke help
.DEFAULT_GOAL := help

help: ## show this
	@grep -hE '^[a-z-]+:.*##' $(MAKEFILE_LIST) | sed 's/:.*##/\t/' | expand -t20

all: $(BIN) ## build sl0ptty

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

test: $(BIN) $(TEST_BIN) $(KDL_TEST) ## unit + headless checks (fast)
	./$(TEST_BIN)
	@echo
	./$(KDL_TEST)
	@echo
	cd tests && python3 test_screen.py && python3 test_layout.py && python3 test_tabs.py && python3 test_osc5577.py && \
		python3 test_responsive.py && python3 test_finder.py && \
		python3 test_config.py && python3 test_layout_files.py && python3 test_drag.py

test-live: $(BIN) ## the checks that need a real tty (slow)
	python3 tests/live_m0.py
	@echo
	python3 tests/live_input.py
	@echo
	python3 tests/test_session.py

test-all: test test-live ## everything

smoke: $(BIN) ## compose a screen headlessly and print it
	./$(BIN) --headless --cols 40 --rows 6 -- /bin/sh -c 'printf "hello \033[1;32msl0ptty\033[0m\n"; echo "wide: 日本語"'

run: $(BIN) ## build and run
	./$(BIN)

clean: ## remove our build output (not the vendor lib)
	rm -rf build
