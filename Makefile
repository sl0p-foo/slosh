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

.PHONY: all clean vendor run test smoke help
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

test: $(BIN) ## run the live pty checks
	python3 tests/live_m0.py

smoke: $(BIN) ## compose a screen headlessly and print it
	./$(BIN) --headless --cols 40 --rows 6 -- /bin/sh -c 'printf "hello \033[1;32msl0ptty\033[0m\n"; echo "wide: 日本語"'

run: $(BIN) ## build and run
	./$(BIN)

clean: ## remove our build output (not the vendor lib)
	rm -rf build
