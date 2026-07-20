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

# Nothing here links against anything -- the panel shells out to all of it. No
# automatic dependency generator will spot these, so a package has to declare
# them by hand, and so does this check.
#
# Astal is deliberately absent: app.js imports none of it, and src/audio.js
# treats AstalWp as an optional fallback. Keeping it as a hard dependency would
# tie the package to the solopasha/hyprland COPR for no gain.
deps:
	@echo "checking runtime dependencies…"
	@command -v gjs >/dev/null && echo "ok   gjs" || echo "MISSING gjs"
	@command -v journalctl >/dev/null \
	    && echo "ok   journalctl (live transcript feed)" \
	    || echo "MISSING journalctl -> no live feed"
	@command -v pactl >/dev/null \
	    && echo "ok   pactl (capture device names)" \
	    || echo "MISSING pactl -> device selection falls back to labels only"
	@command -v parec >/dev/null \
	    && echo "ok   parec (level meter)" \
	    || echo "MISSING parec -> the level meter stays flat"
	@test -e /usr/lib64/girepository-1.0/Gtk4LayerShell-1.0.typelib \
	    && echo "ok   Gtk4LayerShell (optional, --layer only)" \
	    || echo "absent  gtk4-layer-shell -> --layer falls back to a window"
	@fc-list | grep -qi "plex mono" \
	    && echo "ok   IBM Plex Mono" \
	    || echo "MISSING IBM Plex Mono  ->  sudo dnf install ibm-plex-mono-fonts"

# The launcher finds app.js beside itself, which stops being true the moment it
# lands in bin/ -- so point it at the install prefix on the way past.
install:
	install -d $(DESTDIR)$(SHARE)/src
	install -m644 app.js style.css $(DESTDIR)$(SHARE)/
	install -m644 src/*.js $(DESTDIR)$(SHARE)/src/
	install -d $(DESTDIR)$(PREFIX)/bin
	sed 's|@SHAREDIR@|$(SHARE)|' whisprd-menu > $(DESTDIR)$(PREFIX)/bin/whisprd-menu
	chmod 755 $(DESTDIR)$(PREFIX)/bin/whisprd-menu

uninstall:
	rm -rf $(DESTDIR)$(SHARE) $(DESTDIR)$(PREFIX)/bin/whisprd-menu
