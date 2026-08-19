# Build a slosh shader plugin.
#
# The only thing a plugin needs is include/shader_abi.h. Any C compiler will do —
# zig is used here because the project already depends on it, not because it
# has to be. Out of tree, copy shader_abi.h next to your source and drop the
# -I below.

# Not `SLOSH ?=`: slosh itself exports SLOSH=1 into every pane it runs (the
# way tmux exports TMUX), so building *from inside a session* -- the normal
# case -- would read that as the path.
SLOSH_DIR ?= ../..
ZIG     ?= $(HOME)/zig-0.16.0/zig
CC      ?= $(ZIG) cc

CFLAGS  := -std=c23 -O2 -Wall -Wextra -fPIC -I$(SLOSH_DIR)/include
PREFIX  ?= $(HOME)/.config/slosh/shaders

.PHONY: all install clean

all: example.so ## build the plugin

example.so: example.c $(SLOSH_DIR)/include/shader_abi.h
	$(CC) $(CFLAGS) -shared $< -o $@

install: example.so ## copy it where slosh looks
	@mkdir -p $(PREFIX)
	cp example.so $(PREFIX)/
	@echo "installed to $(PREFIX)/example.so — start a new session to load it"

clean:
	rm -f example.so
