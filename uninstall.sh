#!/bin/sh
# scribe uninstaller. Run from the source directory:  ./uninstall.sh
set -eu

PREFIX="${PREFIX:-/usr/local}"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/scribe"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33m    %s\033[0m\n' "$1"; }

[ -f Makefile ] && [ -d src ] || { printf '\033[31merror: run this from the scribe source directory\033[0m\n' >&2; exit 1; }
[ "$(id -u)" -ne 0 ] || { printf '\033[31merror: do not run this as root; it will sudo where needed\033[0m\n' >&2; exit 1; }

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

# ---- done ------------------------------------------------------------------
say "uninstalled"
warn "Note: you were not removed from the 'input' group."
warn "Other tools may rely on it. To remove yourself manually:"
warn "    sudo gpasswd -d $USER input"
