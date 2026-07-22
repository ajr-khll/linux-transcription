#!/bin/sh
# scribe uninstaller. Run from the source directory:  ./uninstall.sh
set -eu

PREFIX="${PREFIX:-/usr/local}"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/scribe"
CONFIG="$CONFIG_DIR/config.ini"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33m    %s\033[0m\n' "$1"; }

[ -f Makefile ] && [ -d src ] || { printf '\033[31merror: run this from the scribe source directory\033[0m\n' >&2; exit 1; }
[ "$(id -u)" -ne 0 ] || { printf '\033[31merror: do not run this as root; it will sudo where needed\033[0m\n' >&2; exit 1; }

# Where the transcripts went. Read now, not at the prompt further down: the
# config is what records a custom history_dir, and by then the question about
# removing the config may already have deleted it. The fallback matches
# history.c, which builds the same path when history_dir is empty.
HISTORY_DIR="$(sed -n 's/^[[:space:]]*history_dir[[:space:]]*=[[:space:]]*//p' \
    "$CONFIG" 2>/dev/null | head -n 1)"
[ -n "$HISTORY_DIR" ] || \
    HISTORY_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/scribe/transcriptions"

# ---- service ---------------------------------------------------------------
say "stopping service"
if systemctl --user is-active --quiet scribe.service 2>/dev/null; then
    systemctl --user stop scribe.service
    echo "    stopped scribe.service"
fi
if systemctl --user is-enabled --quiet scribe.service 2>/dev/null; then
    systemctl --user disable scribe.service
    echo "    disabled scribe.service"
fi
systemctl --user daemon-reload

# ---- installed files -------------------------------------------------------
say "removing installed files"
# The model and the sherpa-onnx runtime go with everything else. That is the
# right outcome -- they are ours and nothing else uses them -- but deleting
# most of a gigabyte without a word is not, so name it first.
MODELS="$PREFIX/share/scribe/models"
if [ -d "$MODELS" ]; then
    warn "also removing $MODELS ($(du -sh "$MODELS" 2>/dev/null | cut -f1) of local models)"
fi
if [ -d "$PREFIX/lib/scribe" ]; then
    warn "also removing $PREFIX/lib/scribe (the sherpa-onnx runtime)"
fi
sudo make uninstall "PREFIX=$PREFIX"
systemctl --user daemon-reload

# ---- udev rule -------------------------------------------------------------
say "udev rule"
RULE=/etc/udev/rules.d/99-scribe.rules
if [ -f "$RULE" ]; then
    sudo rm -f "$RULE"
    sudo udevadm control --reload
    sudo udevadm trigger
    echo "    removed $RULE"
else
    echo "    $RULE not present, nothing to do"
fi

# ---- config ----------------------------------------------------------------
say "configuration"
if [ -d "$CONFIG_DIR" ]; then
    printf '    Remove %s? This deletes your API key and config. [y/N] ' "$CONFIG_DIR"
    read -r REPLY || REPLY=""
    case "$REPLY" in
        [yY]|[yY][eE][sS])
            rm -rf "$CONFIG_DIR"
            echo "    removed $CONFIG_DIR"
            ;;
        *)
            echo "    kept $CONFIG_DIR"
            ;;
    esac
else
    echo "    $CONFIG_DIR not present, nothing to do"
fi

# ---- transcripts -----------------------------------------------------------
# Asked separately from the config, and not folded into it. These live outside
# CONFIG_DIR, so removing that leaves them untouched -- and with history = on
# they are a verbatim record of everything spoken at this machine, which is not
# the same kind of thing as an API key and does not belong behind the same
# question. Say how much there is before asking, so the answer is an informed
# one either way.
say "transcripts"
if [ -d "$HISTORY_DIR" ]; then
    COUNT="$(find "$HISTORY_DIR" -type f 2>/dev/null | wc -l | tr -d '[:space:]')"
    SIZE="$(du -sh "$HISTORY_DIR" 2>/dev/null | cut -f1)"
    echo "    $HISTORY_DIR"
    echo "    $COUNT files, ${SIZE:-unknown size} -- a record of what you dictated."
    printf '    Remove them? [y/N] '
    read -r REPLY || REPLY=""
    case "$REPLY" in
        [yY]|[yY][eE][sS])
            rm -rf "$HISTORY_DIR"
            echo "    removed $HISTORY_DIR"
            # Tidy the parent if scribe was the only thing in it. Never
            # recursive: rmdir refuses a directory with anything left inside.
            rmdir "$(dirname "$HISTORY_DIR")" 2>/dev/null || true
            ;;
        *)
            echo "    kept $HISTORY_DIR"
            ;;
    esac
else
    echo "    $HISTORY_DIR not present, nothing to do"
fi

# ---- done ------------------------------------------------------------------
say "uninstalled"
warn "Note: you were not removed from the 'input' group."
warn "Other tools may rely on it. To remove yourself manually:"
warn "    sudo gpasswd -d $USER input"
