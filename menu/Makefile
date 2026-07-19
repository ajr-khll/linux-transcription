# whisprd-menu -- design 4b floating panel.
# GJS, so nothing compiles; these targets run, check and install it.
# (A justfile with the same targets is provided for `just` users.)

PREFIX ?= /usr/local
SHARE  := $(PREFIX)/share/whisprd-menu

.PHONY: all run run-windowed check deps install uninstall

all: deps

run:
	./whisprd-menu

# Normal window instead of a layer surface -- for X11, or to debug placement.
run-windowed:
	gjs -m app.js

check:
	@gjs -m check.js

# Astal and gtk4-layer-shell are already installed on this machine; the font
# is the one thing usually missing.
deps:
	@echo "checking runtime dependencies…"
	@command -v gjs >/dev/null && echo "ok   gjs" || echo "MISSING gjs"
	@test -e /usr/lib64/girepository-1.0/Astal-4.0.typelib \
	    && echo "ok   Astal-4.0" || echo "MISSING astal-gtk4"
	@command -v pactl >/dev/null \
	    && echo "ok   pactl (capture device names)" \
	    || echo "MISSING pactl -> device selection falls back to labels only"
	@command -v parec >/dev/null \
	    && echo "ok   parec (level meter)" \
	    || echo "MISSING parec -> the level meter stays flat"
	@test -e /usr/lib64/girepository-1.0/Gtk4LayerShell-1.0.typelib \
	    && echo "ok   Gtk4LayerShell" || echo "MISSING gtk4-layer-shell"
	@fc-list | grep -qi "plex mono" \
	    && echo "ok   IBM Plex Mono" \
	    || echo "MISSING IBM Plex Mono  ->  sudo dnf install ibm-plex-mono-fonts"

install:
	install -d $(DESTDIR)$(SHARE)/src
	install -m644 app.js style.css $(DESTDIR)$(SHARE)/
	install -m644 src/*.js $(DESTDIR)$(SHARE)/src/
	install -Dm755 whisprd-menu $(DESTDIR)$(PREFIX)/bin/whisprd-menu
	@echo "installed; set WHISPRD_MENU_DIR=$(SHARE) if it cannot find app.js"

uninstall:
	rm -rf $(DESTDIR)$(SHARE) $(DESTDIR)$(PREFIX)/bin/whisprd-menu
