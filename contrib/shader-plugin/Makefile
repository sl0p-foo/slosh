# Build a slosh shader plugin.
#
# The only thing a plugin needs is src/shader_abi.h. Any C compiler will do —
# zig is used here because the project already depends on it, not because it
# has to be. Out of tree, copy shader_abi.h next to your source and drop the
# -I below.

SLOSH ?= ../..
ZIG     ?= $(HOME)/zig-0.16.0/zig
CC      ?= $(ZIG) cc

CFLAGS  := -std=c23 -O2 -Wall -Wextra -fPIC -I$(SLOSH)/src
PREFIX  ?= $(HOME)/.config/slosh/shaders

.PHONY: all install clean

all: example.so ## build the plugin

example.so: example.c $(SLOSH)/src/shader_abi.h
	$(CC) $(CFLAGS) -shared $< -o $@

install: example.so ## copy it where slosh looks
	@mkdir -p $(PREFIX)
	cp example.so $(PREFIX)/
	@echo "installed to $(PREFIX)/example.so — start a new session to load it"

clean:
	rm -f example.so
