# web/recordings -- render every demo into build/, ready for the site.
#
#   make            every sloshcast (each demos/NAME.py -> build/NAME.json)
#   make serve      look at test.html / seek.html?t=N locally
#   make clean
#
# Rendering needs the slosh binary (the demos drive `slosh --script` through
# tests/harness.py) and nothing else. Casts re-render in about a second, so a
# UI change never leaves a stale demo: re-run make, done.

DEMOS := $(wildcard demos/*.py)
CASTS := $(DEMOS:demos/%.py=build/%.json)
SLOSH := ../../build/slosh

all: $(CASTS)

build/%.json: demos/%.py castgen.py $(SLOSH)
	python3 $< $@

$(SLOSH):
	$(MAKE) -C ../..

PORT ?= 8000
serve:
	@echo "http://localhost:$(PORT)/test.html"
	@python3 -m http.server $(PORT) --bind 127.0.0.1

clean:
	rm -rf build

.PHONY: all serve clean
