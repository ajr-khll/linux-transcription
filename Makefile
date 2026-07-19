VERSION     := 0.1.0
PREFIX      ?= /usr/local
BUILD       ?= build

# Backends are opt-out so users only pull in what their session needs.
WITH_WLR_VK ?= 1
WITH_UINPUT_LAYOUT ?= 1
WITH_GUI    ?= 1

PKGS        := libevdev libpulse-simple libcurl
CFLAGS      ?= -O2 -g
CFLAGS      += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -I$(BUILD)
# Header dependency tracking. Without this, editing a header rebuilds nothing
# that includes it, and a struct layout change silently yields object files
# compiled against different definitions of the same struct.
CFLAGS      += -MMD -MP -DWHISPRD_VERSION='"$(VERSION)"'
CFLAGS      += -DDATADIR='"$(PREFIX)/share"'
LDLIBS      += -lpthread

SRC := src/main.c src/config.c src/input.c src/audio.c src/transcribe.c \
       src/json_text.c src/queue.c src/uinput_kbd.c src/injector.c \
       src/backends/clipboard.c

PROTO_XML   := protocol/virtual-keyboard-unstable-v1.xml
PROTO_H     := $(BUILD)/virtual-keyboard-unstable-v1-client-protocol.h
PROTO_C     := $(BUILD)/virtual-keyboard-unstable-v1-protocol.c

ifeq ($(WITH_UINPUT_LAYOUT),1)
  PKGS   += xkbcommon
  CFLAGS += -DWITH_UINPUT_LAYOUT
  SRC    += src/backends/uinput_layout.c
endif

ifeq ($(WITH_WLR_VK),1)
  PKGS   += wayland-client xkbcommon
  CFLAGS += -DWITH_WLR_VK
  SRC    += src/backends/wlr_vk.c
  GEN    := $(PROTO_C)
endif

PKGS := $(sort $(PKGS))
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC)) $(patsubst $(BUILD)/%.c,$(BUILD)/%.o,$(GEN))

GUI_SRC := src/gui/main.c src/config.c src/audio.c src/queue.c
GUI_CFLAGS := $(CFLAGS) $(shell pkg-config --cflags gtk4)
GUI_LIBS   := $(shell pkg-config --libs gtk4) $(LDLIBS) $(shell pkg-config --libs libpulse-simple libevdev) -lm

ifeq ($(WITH_GUI),1)
  TARGETS += $(BUILD)/whisprd-gui
endif

.PHONY: all clean install uninstall test

all: $(BUILD)/whisprd $(TARGETS)

# Built as its own binary on purpose: the daemon must not depend on GTK.
$(BUILD)/whisprd-gui: $(GUI_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(GUI_CFLAGS) $(GUI_SRC) $(GUI_LIBS) -o $@

# These two cover the parts most likely to be subtly wrong, and need neither a
# compositor nor a network: JSON unescaping, and whether the keymap we generate
# actually produces the characters we meant.
TEST_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
               -Isrc -I$(BUILD) $(shell pkg-config --cflags wayland-client xkbcommon libevdev)
TEST_LIBS   := $(shell pkg-config --libs wayland-client xkbcommon)

test: $(PROTO_H) $(PROTO_C)
	@mkdir -p $(BUILD)
	$(CC) $(TEST_CFLAGS) tests/test_json.c src/json_text.c -o $(BUILD)/test_json
	$(CC) $(TEST_CFLAGS) -DWITH_WLR_VK tests/test_keymap.c $(PROTO_C) $(TEST_LIBS) -o $(BUILD)/test_keymap
	$(CC) $(TEST_CFLAGS) -DWITH_UINPUT_LAYOUT tests/test_layout.c src/uinput_kbd.c src/config.c $(TEST_LIBS) $(shell pkg-config --libs libevdev) -o $(BUILD)/test_layout
	@echo "--- json ---"   && $(BUILD)/test_json
	@echo "--- keymap ---" && $(BUILD)/test_keymap
	@echo "--- layout ---" && $(BUILD)/test_layout

$(BUILD)/whisprd: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) $(LDLIBS) -o $@

# Generated protocol glue must exist before anything that includes it compiles.
$(BUILD)/src/backends/wlr_vk.o: $(PROTO_H)

$(PROTO_H): $(PROTO_XML)
	@mkdir -p $(dir $@)
	wayland-scanner client-header $< $@

$(PROTO_C): $(PROTO_XML)
	@mkdir -p $(dir $@)
	wayland-scanner private-code $< $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJ:.o=.d)

clean:
	rm -rf $(BUILD)

install: all
	install -Dm755 $(BUILD)/whisprd $(DESTDIR)$(PREFIX)/bin/whisprd
	install -Dm644 config.ini.example $(DESTDIR)$(PREFIX)/share/whisprd/config.ini.example
	# The unit is templated at install time; hardcoding /usr/local/bin would
	# ship a broken unit for any non-default PREFIX.
	sed 's|@BINDIR@|$(PREFIX)/bin|' systemd/whisprd.service.in > $(BUILD)/whisprd.service
	install -Dm644 $(BUILD)/whisprd.service $(DESTDIR)$(PREFIX)/lib/systemd/user/whisprd.service
ifeq ($(WITH_GUI),1)
	install -Dm755 $(BUILD)/whisprd-gui $(DESTDIR)$(PREFIX)/bin/whisprd-gui
	install -Dm644 src/gui/style.css $(DESTDIR)$(PREFIX)/share/whisprd/style.css
	install -Dm644 desktop/dev.whisprd.Settings.desktop \
	    $(DESTDIR)$(PREFIX)/share/applications/dev.whisprd.Settings.desktop
endif

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/whisprd $(DESTDIR)$(PREFIX)/bin/whisprd-gui
	rm -f $(DESTDIR)$(PREFIX)/share/applications/dev.whisprd.Settings.desktop
	rm -f $(DESTDIR)$(PREFIX)/lib/systemd/user/whisprd.service
	rm -rf $(DESTDIR)$(PREFIX)/share/whisprd
