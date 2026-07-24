VERSION     := 0.1.0
PREFIX      ?= /usr/local
BUILD       ?= build

# The WITH_* switches below are configure-time choices, not per-command ones,
# so they are remembered here and reused by every later make in this tree.
# Without that, `make install` after `make WITH_PARAKEET=1` quietly rebuilds
# without the local engine and installs that instead -- the flag is on the
# command line that builds and absent from the one that installs, and nothing
# says so. A switch given on the command line still wins, and is remembered in
# its turn; `make clean` forgets the lot.
-include $(BUILD)/config.mk

# Backends are opt-out so users only pull in what their session needs.
WITH_WLR_VK ?= 1
WITH_UINPUT_LAYOUT ?= 1
# The settings UI is scribe-menu (menu/), a separate GJS application. It does
# not compile and does not link against the daemon -- they share only
# config.ini, SIGHUP and the journal -- so it installs rather than builds.
WITH_MENU   ?= 1
# The local engine is opt-in the other way round: it needs ~490 MB downloaded
# by install-parakeet.sh, so a plain `make` must keep working without it.
WITH_PARAKEET ?= 0
SHERPA_PREFIX ?= $(PREFIX)/lib/scribe
SHERPA_INCLUDE ?= third_party/sherpa-onnx/include
PARAKEET_MODEL_DIR ?= $(PREFIX)/share/scribe/models/parakeet-tdt-0.6b-v3-int8
# The live preview's streaming model. Shares WITH_PARAKEET because it is the
# same sherpa-onnx library; install-parakeet.sh fetches both.
LIVE_MODEL_DIR ?= $(PREFIX)/share/scribe/models/streaming-zipformer-en

# `clean` and `uninstall` compile nothing, so nothing under $(BUILD) should be
# written for them -- and `./uninstall.sh` runs `sudo make uninstall`. On a tree
# that was never built that would create build/ owned by root, and the next
# unprivileged make could not write there. An empty goal list means the default
# goal, which does build.
BUILDLESS_GOALS := clean uninstall
BUILDING := $(if $(MAKECMDGOALS),$(filter-out $(BUILDLESS_GOALS),$(MAKECMDGOALS)),all)

# Written only when the choices actually change. Rewriting it every time would
# hand the file to root the first time anyone runs `sudo make install`, and the
# next unprivileged make could not update it.
define CONFIG_MK_TEXT
WITH_WLR_VK := $(WITH_WLR_VK)
WITH_UINPUT_LAYOUT := $(WITH_UINPUT_LAYOUT)
WITH_MENU := $(WITH_MENU)
WITH_PARAKEET := $(WITH_PARAKEET)
endef
ifneq ($(BUILDING),)
ifneq ($(CONFIG_MK_TEXT),$(if $(wildcard $(BUILD)/config.mk),$(file < $(BUILD)/config.mk)))
  $(shell mkdir -p $(BUILD))
  $(file > $(BUILD)/config.mk,$(CONFIG_MK_TEXT))
endif
endif

PKGS        := libevdev libpulse libpulse-simple libcurl
CFLAGS      ?= -O2 -g
CFLAGS      += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -I$(BUILD)
# Header dependency tracking. Without this, editing a header rebuilds nothing
# that includes it, and a struct layout change silently yields object files
# compiled against different definitions of the same struct.
CFLAGS      += -MMD -MP -DSCRIBE_VERSION='"$(VERSION)"'
LDLIBS      += -lpthread -lm

SRC := src/main.c src/config.c src/input.c src/audio.c \
       src/transcribe.c src/asr_openai.c src/polish.c \
       src/json_text.c src/queue.c src/uinput_kbd.c src/injector.c \
       src/history.c src/cue.c src/vad.c src/live.c src/screen.c \
       src/confwatch.c src/backends/clipboard.c

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

ifeq ($(WITH_PARAKEET),1)
  CFLAGS += -DWITH_PARAKEET -I$(SHERPA_INCLUDE) \
            -DSCRIBE_MODEL_DIR='"$(PARAKEET_MODEL_DIR)"' \
            -DSCRIBE_LIVE_MODEL_DIR='"$(LIVE_MODEL_DIR)"'
  SRC    += src/asr_parakeet.c src/asr_stream.c
  # sherpa-onnx is not packaged by any distribution and has no .pc file, so
  # there is nothing for pkg-config to find. rpath rather than ldconfig: the
  # library is ours, it lives under our own prefix, and a global search path
  # entry is not ours to add. libsherpa-onnx-c-api.so carries RPATH=$$ORIGIN,
  # so it finds libonnxruntime.so beside it without further help.
  LDLIBS += -L$(SHERPA_PREFIX) -lsherpa-onnx-c-api \
            -Wl,-rpath,$(PREFIX)/lib/scribe
endif

PKGS := $(sort $(PKGS))
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC)) $(patsubst $(BUILD)/%.c,$(BUILD)/%.o,$(GEN))

# Make compares timestamps, not command lines, so changing a WITH_* flag
# rebuilds only the files that flag adds or removes and relinks the rest as
# they were. `make WITH_PARAKEET=1` over a plain build therefore produced a
# binary that contained the whole local engine and a transcribe.o compiled
# without -DWITH_PARAKEET, which refuses to use it -- a working engine and a
# refusal to run it, in one executable, with nothing said about why.
#
# So record the flags in a file that every object depends on, rewritten only
# when they actually change. $(file) rather than a shell redirect because
# CFLAGS carries quotes that a shell would eat.
BUILD_FLAGS := $(CFLAGS) $(LDLIBS)
FLAGS_STAMP := $(BUILD)/.build-flags
PREV_FLAGS  := $(if $(wildcard $(FLAGS_STAMP)),$(file < $(FLAGS_STAMP)))
ifneq ($(BUILDING),)
ifneq ($(BUILD_FLAGS),$(PREV_FLAGS))
  $(shell mkdir -p $(BUILD))
  $(file > $(FLAGS_STAMP),$(BUILD_FLAGS))
endif
endif

.PHONY: all clean install uninstall test test-parakeet test-stream

all: $(BUILD)/scribe

# These cover the parts most likely to be subtly wrong, and need neither a
# compositor, a network, nor a microphone: JSON unescaping, whether the keymap
# we generate actually produces the characters we meant, and whether the speech
# detector can tell a syllable from a hiss.
TEST_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
               -Isrc -I$(BUILD) $(shell pkg-config --cflags wayland-client xkbcommon libevdev)
TEST_LIBS   := $(shell pkg-config --libs wayland-client xkbcommon)

test: $(PROTO_H) $(PROTO_C)
	@mkdir -p $(BUILD)
	$(CC) $(TEST_CFLAGS) tests/test_json.c src/json_text.c -o $(BUILD)/test_json
	$(CC) $(TEST_CFLAGS) -DWITH_WLR_VK tests/test_keymap.c $(PROTO_C) $(TEST_LIBS) -o $(BUILD)/test_keymap
	$(CC) $(TEST_CFLAGS) -DWITH_UINPUT_LAYOUT tests/test_layout.c src/uinput_kbd.c src/config.c $(TEST_LIBS) $(shell pkg-config --libs libevdev) -o $(BUILD)/test_layout
	$(CC) $(TEST_CFLAGS) tests/test_vad.c src/vad.c -lm -o $(BUILD)/test_vad
	$(CC) $(TEST_CFLAGS) tests/test_live.c src/config.c $(shell pkg-config --libs libevdev) -lpthread -o $(BUILD)/test_live
	$(CC) $(TEST_CFLAGS) tests/test_screen.c src/screen.c -o $(BUILD)/test_screen
	$(CC) $(TEST_CFLAGS) -DSCRIBE_VERSION='"$(VERSION)"' tests/test_polish.c src/polish.c src/json_text.c $(shell pkg-config --libs libcurl) -o $(BUILD)/test_polish
	@echo "--- json ---"   && $(BUILD)/test_json
	@echo "--- keymap ---" && $(BUILD)/test_keymap
	@echo "--- layout ---" && $(BUILD)/test_layout
	@echo "--- vad ---"    && $(BUILD)/test_vad
	@echo "--- live ---"   && $(BUILD)/test_live
	@echo "--- screen ---" && $(BUILD)/test_screen
	@echo "--- polish ---" && $(BUILD)/test_polish

# Separate from `test` on purpose: it needs a 640 MB model and a library that
# a plain checkout does not have. Pass a WAV to run it -- install-parakeet.sh
# keeps one beside the model as test.wav.
test-parakeet:
	@mkdir -p $(BUILD)
	$(CC) $(TEST_CFLAGS) -DWITH_PARAKEET -I$(SHERPA_INCLUDE) \
	    -DSCRIBE_MODEL_DIR='"$(PARAKEET_MODEL_DIR)"' \
	    tests/test_parakeet.c src/asr_parakeet.c src/config.c \
	    -L$(SHERPA_PREFIX) -lsherpa-onnx-c-api -Wl,-rpath,$(PREFIX)/lib/scribe \
	    $(shell pkg-config --libs libevdev) -o $(BUILD)/test_parakeet
	@echo "run: $(BUILD)/test_parakeet $(PARAKEET_MODEL_DIR)/test.wav"

# The live engine, same deal. Reports how often the streaming model revises
# text it already emitted, which is what decides whether live mode is bearable:
# every revision is backspaces sent to whatever window has focus.
test-stream:
	@mkdir -p $(BUILD)
	$(CC) $(TEST_CFLAGS) -DWITH_PARAKEET -I$(SHERPA_INCLUDE) \
	    -DSCRIBE_LIVE_MODEL_DIR='"$(LIVE_MODEL_DIR)"' \
	    tests/test_stream.c src/asr_stream.c src/config.c \
	    -L$(SHERPA_PREFIX) -lsherpa-onnx-c-api -Wl,-rpath,$(PREFIX)/lib/scribe \
	    $(shell pkg-config --libs libevdev) -o $(BUILD)/test_stream
	@echo "run: $(BUILD)/test_stream $(LIVE_MODEL_DIR)/test.wav"

$(BUILD)/scribe: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) $(LDLIBS) -o $@

# Every object depends on the flags it was compiled with -- see FLAGS_STAMP
# above. This must come after `all`, or the first object becomes the default
# goal and a bare `make` builds one file and stops.
$(OBJ): $(FLAGS_STAMP)

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
	install -Dm755 $(BUILD)/scribe $(DESTDIR)$(PREFIX)/bin/scribe
	install -Dm644 config.ini.example $(DESTDIR)$(PREFIX)/share/scribe/config.ini.example
	# The unit is templated at install time; hardcoding /usr/local/bin would
	# ship a broken unit for any non-default PREFIX.
	sed 's|@BINDIR@|$(PREFIX)/bin|' systemd/scribe.service.in > $(BUILD)/scribe.service
	install -Dm644 $(BUILD)/scribe.service $(DESTDIR)$(PREFIX)/lib/systemd/user/scribe.service
ifeq ($(WITH_MENU),1)
	$(MAKE) -C menu install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	install -Dm644 desktop/dev.scribe.Menu.desktop \
	    $(DESTDIR)$(PREFIX)/share/applications/dev.scribe.Menu.desktop
endif

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/scribe
	rm -f $(DESTDIR)$(PREFIX)/share/applications/dev.scribe.Menu.desktop
	rm -f $(DESTDIR)$(PREFIX)/lib/systemd/user/scribe.service
	# This takes the Parakeet model with it -- 640 MB, silently. uninstall.sh
	# says so first; a bare `make uninstall` does not, and cannot.
	rm -rf $(DESTDIR)$(PREFIX)/share/scribe
	rm -rf $(DESTDIR)$(PREFIX)/lib/scribe
	$(MAKE) -C menu uninstall PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
