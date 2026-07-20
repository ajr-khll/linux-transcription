#!/bin/sh
# whisprd installer. Run from a checkout:  ./install.sh
#
# Deliberately not a curl|bash one-liner. This needs sudo for /usr/local and
# for the input group, and piping an unread script into a shell that then asks
# for your password is a habit worth not teaching.
set -eu

PREFIX="${PREFIX:-/usr/local}"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/whisprd"
CONFIG="$CONFIG_DIR/config.ini"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33m    %s\033[0m\n' "$1"; }
die()  { printf '\033[31merror: %s\033[0m\n' "$1" >&2; exit 1; }

[ -f Makefile ] && [ -d src ] || die "run this from the whisprd source directory"
[ "$(id -u)" -ne 0 ] || die "do not run this as root; it will sudo where needed"

# ---- dependencies ----------------------------------------------------------
# Build deps for the daemon, then the panel's runtime deps. The panel links
# against none of its own -- it shells out to gjs, pactl, parec and journalctl
# -- so nothing here is discovered automatically and all of it must be named.
say "installing dependencies"
if command -v dnf >/dev/null; then
    sudo dnf install -y \
        gcc make pkgconf-pkg-config \
        libevdev-devel pulseaudio-libs-devel libcurl-devel \
        libxkbcommon-devel wayland-devel wayland-protocols-devel \
        gjs gtk4 pulseaudio-utils ibm-plex-mono-fonts
elif command -v apt >/dev/null; then
    sudo apt update
    sudo apt install -y \
        build-essential pkg-config \
        libevdev-dev libpulse-dev libcurl4-openssl-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols \
        gjs libgtk-4-1 pulseaudio-utils fonts-ibm-plex
elif command -v pacman >/dev/null; then
    sudo pacman -S --needed --noconfirm \
        base-devel libevdev libpulse curl libxkbcommon wayland wayland-protocols \
        gjs gtk4 libpulse ttf-ibm-plex
else
    warn "unknown distribution -- install the deps in README.md by hand."
    warn "continuing; the build will tell you what is missing."
fi

# ---- build -----------------------------------------------------------------
say "building"
make
make test

say "installing to $PREFIX"
sudo make install "PREFIX=$PREFIX"

# ---- permissions -----------------------------------------------------------
# whisprd reads /dev/input/event* and writes /dev/uinput. It must NOT run as
# root, so the user joins the input group instead.
say "permissions"
if id -nG | tr ' ' '\n' | grep -qx input; then
    echo "    already in the input group"
    NEED_LOGOUT=0
else
    sudo usermod -aG input "$USER"
    warn "added $USER to the input group"
    NEED_LOGOUT=1
fi

if ! stat -c '%G' /dev/uinput 2>/dev/null | grep -qx input; then
    warn "/dev/uinput is not group 'input'; installing the udev rule"
    sudo cp udev/99-whisprd.rules /etc/udev/rules.d/
    sudo udevadm control --reload && sudo udevadm trigger
fi

# ---- config ----------------------------------------------------------------
say "configuration"
mkdir -p "$CONFIG_DIR"
chmod 700 "$CONFIG_DIR"
if [ -f "$CONFIG" ]; then
    echo "    keeping existing $CONFIG"
else
    cp "$PREFIX/share/whisprd/config.ini.example" "$CONFIG"
    chmod 600 "$CONFIG"
    echo "    wrote $CONFIG"
fi

# The key is required -- the daemon refuses to start without one -- so ask now
# rather than let the first run fail. Read with the terminal echo off, and
# written with an existing-key check so re-running the installer is safe.
if [ -n "${OPENAI_API_KEY:-}" ]; then
    echo "    using OPENAI_API_KEY from the environment"
elif grep -qE '^[[:space:]]*api_key[[:space:]]*=[[:space:]]*sk-' "$CONFIG"; then
    echo "    key already set in $CONFIG"
elif [ -t 0 ]; then
    printf '    OpenAI API key (https://platform.openai.com/api-keys)\n'
    printf '    paste it here, or leave blank to fill in later: '
    stty -echo 2>/dev/null || true
    read -r KEY || KEY=""
    stty echo 2>/dev/null || true
    printf '\n'
    if [ -n "$KEY" ]; then
        # The key goes through the environment, not through the script text.
        # Interpolating it into a sed replacement would let '|', '&' or a
        # backslash in the key corrupt it silently -- '&' expands to the whole
        # matched line -- and would put it in the process table for anyone
        # running ps. awk reads it from ENVIRON and treats it as plain data.
        tmp="$(mktemp)"
        chmod 600 "$tmp"
        WHISPRD_KEY="$KEY" awk '
            /^[[:space:]]*api_key[[:space:]]*=/ && !done {
                print "api_key      = " ENVIRON["WHISPRD_KEY"]; done = 1; next
            }
            { print }
        ' "$CONFIG" > "$tmp"
        mv "$tmp" "$CONFIG"
        chmod 600 "$CONFIG"
        echo "    key written to $CONFIG (0600)"
    else
        warn "no key set; whisprd will not start until you add one"
    fi
else
    warn "no key set; whisprd will not start until you add one"
fi

# ---- service ---------------------------------------------------------------
# The unit is WantedBy=graphical-session.target. GNOME and KDE reach that
# reliably; bare Hyprland/sway/river often never activate it, in which case
# `enable` links the unit and nothing ever starts it.
say "service"
systemctl --user daemon-reload
systemctl --user enable whisprd.service

if systemctl --user is-active --quiet graphical-session.target; then
    if [ "${NEED_LOGOUT:-0}" -eq 0 ]; then
        systemctl --user restart whisprd.service || \
            warn "the unit did not start; check: journalctl --user -u whisprd -n 30"
    fi
else
    warn "graphical-session.target is not active on this session."
    warn "The unit is enabled but your compositor will never start it."
    warn "Either launch your compositor under uwsm, or add to its autostart:"
    warn "    systemctl --user start whisprd.service"
fi

# ---- done ------------------------------------------------------------------
say "installed"
cat <<EOF
    whisprd        the daemon (systemd user service)
    whisprd-menu   settings and history panel

Next:
    whisprd --list-sources     pick a microphone, set 'source' in the config
    whisprd --say "hello"      test injection without speaking
    whisprd-menu               open the panel

Bind whisprd-menu to a key in your compositor -- see menu/README.md.
EOF

if [ "${NEED_LOGOUT:-0}" -eq 1 ]; then
    printf '\n\033[33m'
    echo "You must log out and back in before whisprd can run."
    echo "Group membership is read when your session starts, so the service"
    echo "will fail on /dev/uinput until then. A new shell is not enough."
    printf '\033[0m'
fi
