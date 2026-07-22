#!/bin/sh
# scribe installer. Run from a checkout:  ./install.sh
#
# Deliberately not a curl|bash one-liner. This needs sudo for /usr/local and
# for the input group, and piping an unread script into a shell that then asks
# for your password is a habit worth not teaching.
set -eu

PREFIX="${PREFIX:-/usr/local}"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/scribe"
CONFIG="$CONFIG_DIR/config.ini"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/scribe"
OLD_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/whisprd"
OLD_DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/whisprd"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33m    %s\033[0m\n' "$1"; }
die()  { printf '\033[31merror: %s\033[0m\n' "$1" >&2; exit 1; }

[ -f Makefile ] && [ -d src ] || die "run this from the scribe source directory"
[ "$(id -u)" -ne 0 ] || die "do not run this as root; it will sudo where needed"

# Rewrites `key = value` in $CONFIG, appending the line if it is not there.
#
# The value goes through the environment, not through the script text.
# Interpolating it into a sed replacement would let '|', '&' or a backslash in
# the value corrupt it silently -- '&' expands to the whole matched line -- and
# would put it in the process table for anyone running ps. awk reads it from
# ENVIRON and treats it as plain data.
set_config_key() {
    _tmp="$(mktemp)"
    chmod 600 "$_tmp"
    SCRIBE_KEY="$1" SCRIBE_VALUE="$2" awk '
        BEGIN { key = ENVIRON["SCRIBE_KEY"] }
        $0 ~ "^[[:space:]]*" key "[[:space:]]*=" && !done {
            print key " = " ENVIRON["SCRIBE_VALUE"]; done = 1; next
        }
        { print }
        END { if (!done) print key " = " ENVIRON["SCRIBE_VALUE"] }
    ' "$CONFIG" > "$_tmp"
    mv "$_tmp" "$CONFIG"
    chmod 600 "$CONFIG"
}

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

# ---- migration from whisprd ------------------------------------------------
# The project was called whisprd before 0.2.0. Move the old config and
# transcripts across, or an upgrade starts from an empty config and leaves a
# working API key stranded in a directory nothing reads any more.
#
# mv rather than cp: config.ini is 0600 because it holds the key, and mv keeps
# the mode where cp would apply the umask. Nothing is ever overwritten -- if
# both exist the new one wins and the old is left alone for you to inspect.
# This runs before the engine and config sections below, so both read the
# migrated file rather than a fresh empty one.
migrate() {
    _old="$1"
    _new="$2"
    [ -e "$_old" ] || return 0
    if [ -e "$_new" ]; then
        warn "both $_old and $_new exist; keeping $_new"
        warn "the old one is untouched -- remove it once you have checked it"
        return 0
    fi
    mkdir -p "$(dirname "$_new")"
    mv "$_old" "$_new"
    echo "    moved $_old -> $_new"
}

if [ -e "$OLD_CONFIG_DIR" ] || [ -e "$OLD_DATA_DIR" ]; then
    say "migrating from whisprd"
    migrate "$OLD_CONFIG_DIR" "$CONFIG_DIR"
    migrate "$OLD_DATA_DIR" "$DATA_DIR"
    if [ -f "$CONFIG" ]; then
        chmod 600 "$CONFIG"
    fi
fi

# ---- engine ----------------------------------------------------------------
# Asked here rather than with the rest of the config, because the answer
# changes how the daemon compiles: the local engine is a build flag, not a
# runtime switch.
say "transcription engine"
ENGINE=openai
if [ -f "$CONFIG" ] && grep -qE '^[[:space:]]*engine[[:space:]]*=[[:space:]]*parakeet' "$CONFIG"; then
    # Same reasoning as the API key check below: a re-run should not re-ask a
    # question the config already answers.
    ENGINE=parakeet
    echo "    keeping engine = parakeet from $CONFIG"
elif [ -f "$CONFIG" ] && grep -qE '^[[:space:]]*engine[[:space:]]*=[[:space:]]*openai' "$CONFIG"; then
    echo "    keeping engine = openai from $CONFIG"
elif [ -t 0 ]; then
    cat <<'EOF'
    1) local   Parakeet runs on this machine. No account, no key, no bill,
               and nothing you dictate leaves the computer. Costs a 490 MB
               download, about 1 GB of memory while running, and covers 25
               European languages.
    2) cloud   OpenAI Whisper. Needs an API key and a card on file, and
               every utterance is uploaded. Covers about 99 languages.
EOF
    printf '    pick 1 or 2 [1]: '
    read -r REPLY || REPLY=""
    case "$REPLY" in
        2) ENGINE=openai ;;
        *) ENGINE=parakeet ;;
    esac
else
    warn "not a terminal; defaulting to the cloud engine"
fi
echo "    engine: $ENGINE"

MAKE_ARGS="PREFIX=$PREFIX"
if [ "$ENGINE" = parakeet ]; then
    ./install-parakeet.sh
    MAKE_ARGS="$MAKE_ARGS WITH_PARAKEET=1"
fi

# ---- build -----------------------------------------------------------------
say "building"
# shellcheck disable=SC2086  # MAKE_ARGS is a word list on purpose
make $MAKE_ARGS
make test

say "installing to $PREFIX"
# The same arguments as the build: a different flag here would relink the
# binary without the engine that was just downloaded for it.
sudo make install $MAKE_ARGS

# ---- permissions -----------------------------------------------------------
# scribe reads /dev/input/event* and writes /dev/uinput. It must NOT run as
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
    sudo cp udev/99-scribe.rules /etc/udev/rules.d/
    sudo udevadm control --reload && sudo udevadm trigger
fi

# ---- config ----------------------------------------------------------------
say "configuration"
mkdir -p "$CONFIG_DIR"
chmod 700 "$CONFIG_DIR"
if [ -f "$CONFIG" ]; then
    echo "    keeping existing $CONFIG"
else
    cp "$PREFIX/share/scribe/config.ini.example" "$CONFIG"
    chmod 600 "$CONFIG"
    echo "    wrote $CONFIG"
fi

set_config_key engine "$ENGINE"
echo "    engine = $ENGINE"

# The local engine needs no key at all, so do not ask for one. Asking anyway is
# how a program teaches people that its questions can be ignored.
if [ "$ENGINE" = parakeet ]; then
    echo "    no API key needed"
# The key is required -- the daemon refuses to start without one -- so ask now
# rather than let the first run fail. Read with the terminal echo off, and
# written with an existing-key check so re-running the installer is safe.
elif [ -n "${OPENAI_API_KEY:-}" ]; then
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
        set_config_key api_key "$KEY"
        echo "    key written to $CONFIG (0600)"
    else
        warn "no key set; scribe will not start until you add one"
    fi
else
    warn "no key set; scribe will not start until you add one"
fi

# ---- service ---------------------------------------------------------------
# The unit is WantedBy=graphical-session.target. GNOME and KDE reach that
# reliably; bare Hyprland/sway/river often never activate it, in which case
# `enable` links the unit and nothing ever starts it.
say "service"
systemctl --user daemon-reload
systemctl --user enable scribe.service

if systemctl --user is-active --quiet graphical-session.target; then
    if [ "${NEED_LOGOUT:-0}" -eq 0 ]; then
        systemctl --user restart scribe.service || \
            warn "the unit did not start; check: journalctl --user -u scribe -n 30"
    fi
else
    warn "graphical-session.target is not active on this session."
    warn "The unit is enabled but your compositor will never start it."
    warn "Either launch your compositor under uwsm, or add to its autostart:"
    warn "    systemctl --user start scribe.service"
fi

# ---- done ------------------------------------------------------------------
say "installed"
cat <<EOF
    scribe        the daemon (systemd user service)
    scribe-menu   settings and history panel

Next:
    scribe --list-sources     pick a microphone, set 'source' in the config
    scribe --say "hello"      test injection without speaking
    scribe-menu               open the panel

Bind scribe-menu to a key in your compositor -- see menu/README.md.
EOF

if [ "${NEED_LOGOUT:-0}" -eq 1 ]; then
    printf '\n\033[33m'
    echo "You must log out and back in before scribe can run."
    echo "Group membership is read when your session starts, so the service"
    echo "will fail on /dev/uinput until then. A new shell is not enough."
    printf '\033[0m'
fi
