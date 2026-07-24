#!/bin/sh
# scribe installer. Run from a checkout:  ./install.sh
#
# Deliberately not a curl|bash one-liner. This needs sudo for /usr/local and
# for the input group, and piping an unread script into a shell that then asks
# for your password is a habit worth not teaching.
#
# Every question is asked before any work starts, and every one of them can be
# answered on the command line instead -- see --help. Nothing is asked halfway
# through a ten-minute build.
set -eu

PREFIX="${PREFIX:-/usr/local}"
USER_NAME="${USER:-$(id -un)}"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33m    %s\033[0m\n' "$1"; }
die()  { printf '\033[31merror: %s\033[0m\n' "$1" >&2; exit 1; }

usage() {
    cat <<'EOF'
usage: ./install.sh [options]

Installs scribe: build dependencies, the daemon, the settings panel, the input
group, your config and the systemd user service.

  --engine=local     transcribe on this machine. Parakeet, ~560 MB download,
                     no account and nothing leaves the computer. The default.
  --engine=cloud     transcribe via OpenAI. Needs an API key, and every
                     utterance is uploaded.
  --api-key=KEY      the OpenAI key, for --engine=cloud. $OPENAI_API_KEY in
                     the environment does the same and keeps it out of your
                     shell history.
  --prefix=DIR       install root: /usr/local (the default) or /usr. Anywhere
                     else and systemd never finds the service -- see 'By hand'
                     in README.md to install elsewhere and start it yourself.
  --skip-deps        do not touch the package manager.
  -y, --yes          take the default for every question and ask nothing.
  -h, --help         this.

Run with no options it asks instead. Answers already in your config are kept,
so re-running it is safe and changes only what you tell it to.
EOF
}

# ---- arguments -------------------------------------------------------------
ENGINE=
SKIP_DEPS=0
ASSUME_YES=0

# A key from the environment is left there. The README recommends OPENAI_API_KEY
# over the config file for anyone who syncs their dotfiles, and copying it into
# a file behind their back would undo exactly that.
API_KEY=
KEY_FROM_ENV=0
if [ -n "${OPENAI_API_KEY:-}" ]; then
    API_KEY="$OPENAI_API_KEY"
    KEY_FROM_ENV=1
fi

for arg do
    case "$arg" in
        --engine=local|--engine=parakeet)  ENGINE=parakeet ;;
        --engine=cloud|--engine=openai)    ENGINE=openai ;;
        --engine=*) die "unknown engine '${arg#--engine=}' -- use local or cloud" ;;
        --api-key=*) API_KEY="${arg#--api-key=}"; KEY_FROM_ENV=0 ;;
        --prefix=*)  PREFIX="${arg#--prefix=}" ;;
        --skip-deps) SKIP_DEPS=1 ;;
        -y|--yes)    ASSUME_YES=1 ;;
        -h|--help)   usage; exit 0 ;;
        *) usage >&2; die "unknown option: $arg" ;;
    esac
done

# /usr/local/ is the same place as /usr/local but not the same string, and tab
# completion supplies the slash for you. Strip it, or the check below rejects a
# prefix that is perfectly good.
while [ "$PREFIX" != "/" ] && [ "${PREFIX%/}" != "$PREFIX" ]; do
    PREFIX="${PREFIX%/}"
done

# install-parakeet.sh is a separate script and reads PREFIX from the
# environment. A --prefix given here is a plain shell variable, which a child
# never sees -- so the model would land in /usr/local while the daemon was
# built to look somewhere else, and only fail at the first utterance.
export PREFIX

CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/scribe"
CONFIG="$CONFIG_DIR/config.ini"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/scribe"
OLD_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/whisprd"
OLD_DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/whisprd"

[ -f Makefile ] && [ -d src ] || die "run this from the scribe source directory"
[ "$(id -u)" -ne 0 ] || die "do not run this as root; it will sudo where needed"

# ---- prefix ----------------------------------------------------------------
# `make install` puts the systemd user unit in $PREFIX/lib/systemd/user, and
# systemd searches a fixed set of directories rather than looking around. Point
# PREFIX anywhere else -- /opt, a home directory -- and every step here
# succeeds, this script says "installed", and systemd then reports the unit
# does not exist, with nothing to connect the two.
#
# Ask systemd for the list rather than guessing at it; the set differs between
# versions and distributions. systemd-analyze needs no session bus for this and
# should always be present, but fall back to the two prefixes whose unit
# directory is compiled into every systemd.
prefix_ok() {
    if command -v systemd-analyze >/dev/null 2>&1; then
        systemd-analyze --user unit-paths 2>/dev/null \
            | grep -qxF "$PREFIX/lib/systemd/user"
        return $?
    fi
    case "$PREFIX" in /usr/local|/usr) return 0 ;; esac
    return 1
}

prefix_ok || die "PREFIX=$PREFIX will not work.
    systemd does not look for user units in $PREFIX/lib/systemd/user, so the
    service would install and then never be found. Use /usr/local (the default)
    or /usr.

    Installing under your home directory is not supported either way: this
    script needs root for the input group and the udev rule. To put scribe
    somewhere else and start it yourself, see 'By hand' in README.md."

# True when there is someone at the other end to answer a question. --yes says
# there is not, even on a terminal.
asking() { [ "$ASSUME_YES" -eq 0 ] && [ -t 0 ]; }

# Reads `key = value` out of $CONFIG. Empty if the file or the key is absent.
config_value() {
    [ -f "$CONFIG" ] || return 0
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONFIG" | head -n 1
}

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

# ---- migration from whisprd ------------------------------------------------
# The project was called whisprd before 0.2.0. Move the old config and
# transcripts across, or an upgrade starts from an empty config and leaves a
# working API key stranded in a directory nothing reads any more.
#
# mv rather than cp: config.ini is 0600 because it holds the key, and mv keeps
# the mode where cp would apply the umask. Nothing is ever overwritten -- if
# both exist the new one wins and the old is left alone for you to inspect.
# This runs first, before any question, so the questions below read the
# migrated config rather than deciding there is none.
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
    [ -f "$CONFIG" ] && chmod 600 "$CONFIG"
fi

# ---- question: which engine ------------------------------------------------
# Asked first, because the answer changes how the daemon compiles -- the local
# engine is a build flag, not a runtime switch -- and because it decides
# whether there is any point asking for an API key.
say "transcription engine"
CONFIGURED_ENGINE="$(config_value engine)"
if [ -n "$ENGINE" ]; then
    echo "    $ENGINE, from the command line"
elif [ "$CONFIGURED_ENGINE" = parakeet ] || [ "$CONFIGURED_ENGINE" = openai ]; then
    # A re-run should not re-ask a question the config already answers.
    ENGINE="$CONFIGURED_ENGINE"
    echo "    keeping engine = $ENGINE from $CONFIG"
elif asking; then
    cat <<'EOF'
    1) local   Parakeet runs on this machine. No account, no key, no bill,
               and nothing you dictate leaves the computer. Costs a 560 MB
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
    echo "    engine: $ENGINE"
else
    ENGINE=parakeet
    echo "    local (the default; --engine=cloud for OpenAI)"
fi

# ---- question: the API key -------------------------------------------------
# The cloud engine will not start without one, so ask now rather than let the
# first run fail. The local engine needs no key at all and is not asked for
# one: asking anyway is how a program teaches people its questions can be
# ignored.
if [ "$ENGINE" = openai ]; then
    say "OpenAI API key"
    if [ "$KEY_FROM_ENV" -eq 1 ]; then
        echo "    using OPENAI_API_KEY from the environment; not copied to disk"
    elif [ -n "$API_KEY" ]; then
        echo "    supplied"
    elif [ -n "$(config_value api_key)" ]; then
        echo "    already set in $CONFIG"
    elif asking; then
        echo "    get one at https://platform.openai.com/api-keys"
        printf '    paste it here, or leave blank to fill in later: '
        # Restore the terminal even if this is interrupted mid-read, or the
        # shell is left with echo off and no sign of why.
        trap 'stty echo 2>/dev/null || true' INT TERM
        stty -echo 2>/dev/null || true
        read -r API_KEY || API_KEY=""
        stty echo 2>/dev/null || true
        trap - INT TERM
        printf '\n'
        [ -n "$API_KEY" ] || warn "none given; scribe will not start until you add one"
    else
        warn "no key given; scribe will not start until you add one"
        warn "pass --api-key=... or set OPENAI_API_KEY"
    fi
fi

# ---- the plan --------------------------------------------------------------
# Root is needed four times over the next several minutes. Say what for, and
# take the password once here, rather than surprising someone with a prompt
# from inside a build.
say "what this will do"
STEP=1
if [ "$SKIP_DEPS" -eq 0 ]; then
    echo "    $STEP. install build dependencies (root)"; STEP=$((STEP + 1))
fi
if [ "$ENGINE" = parakeet ]; then
    echo "    $STEP. download the local engine, ~560 MB, to $PREFIX/share/scribe (root)"
    STEP=$((STEP + 1))
fi
echo "    $STEP. build scribe and run its tests"; STEP=$((STEP + 1))
echo "    $STEP. install it to $PREFIX (root)"; STEP=$((STEP + 1))
echo "    $STEP. add $USER_NAME to the 'input' group if not already (root)"; STEP=$((STEP + 1))
echo "    $STEP. write $CONFIG"; STEP=$((STEP + 1))
echo "    $STEP. enable the scribe systemd user service"
echo
echo "    Nothing else is asked. Ctrl-C now if that is not what you want."

sudo -v || die "root is required for the steps marked (root)"

# ---- dependencies ----------------------------------------------------------
# Build deps for the daemon, then the panel's runtime deps. The panel links
# against none of its own -- it shells out to gjs, pactl, parec and journalctl
# -- so nothing here is discovered automatically and all of it must be named.
# curl and bzip2 are for install-parakeet.sh, which fetches a .tar.bz2 (GNU tar
# execs bzip2 rather than decompressing it itself).
if [ "$SKIP_DEPS" -eq 1 ]; then
    say "dependencies"
    echo "    skipped"
elif command -v dnf >/dev/null; then
    say "installing dependencies with dnf"
    sudo dnf install -y \
        gcc make pkgconf-pkg-config curl bzip2 \
        libevdev-devel pulseaudio-libs-devel libcurl-devel \
        libxkbcommon-devel wayland-devel wayland-protocols-devel \
        gjs gtk4 pulseaudio-utils ibm-plex-mono-fonts
elif command -v apt >/dev/null; then
    say "installing dependencies with apt"
    sudo apt update
    # gir1.2-gtk-4.0, not just libgtk-4-1: scribe-menu is GJS and reaches GTK
    # through GObject introspection, so it needs the typelib. Without it the
    # panel dies on its first import and the daemon looks fine.
    sudo apt install -y \
        build-essential pkg-config curl bzip2 \
        libevdev-dev libpulse-dev libcurl4-openssl-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols \
        gjs libgtk-4-1 gir1.2-gtk-4.0 pulseaudio-utils fonts-ibm-plex
elif command -v pacman >/dev/null; then
    say "installing dependencies with pacman"
    sudo pacman -S --needed --noconfirm \
        base-devel libevdev libpulse curl bzip2 libxkbcommon \
        wayland wayland-protocols gjs gtk4 ttf-ibm-plex
else
    say "dependencies"
    warn "unknown distribution -- install the deps in README.md by hand."
    warn "continuing; the build will tell you what is missing."
fi

# Whatever the package manager did or did not do, these four have to be here or
# the failure lands somewhere much less obvious.
for tool in cc make pkg-config wayland-scanner; do
    command -v "$tool" >/dev/null || die "$tool is missing -- see the dependency list in README.md"
done

MAKE_ARGS="PREFIX=$PREFIX"
if [ "$ENGINE" = parakeet ]; then
    ./install-parakeet.sh
    MAKE_ARGS="$MAKE_ARGS WITH_PARAKEET=1"
fi

# ---- build -----------------------------------------------------------------
say "building"
# MAKE_ARGS is a word list on purpose, hence the unquoted expansions below.
# shellcheck disable=SC2086
make -j"$(nproc 2>/dev/null || echo 2)" $MAKE_ARGS
# shellcheck disable=SC2086
make $MAKE_ARGS test

say "installing to $PREFIX"
# The same arguments as the build: a different flag here would relink the
# binary without the engine that was just downloaded for it.
# shellcheck disable=SC2086
sudo make install $MAKE_ARGS

# ---- permissions -----------------------------------------------------------
# scribe reads /dev/input/event* and writes /dev/uinput. It must NOT run as
# root, so the user joins the input group instead.
say "permissions"
if id -nG | tr ' ' '\n' | grep -qx input; then
    echo "    already in the input group"
    NEED_LOGOUT=0
else
    sudo usermod -aG input "$USER_NAME"
    warn "added $USER_NAME to the input group"
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

if [ "$ENGINE" = parakeet ]; then
    echo "    no API key needed"
elif [ "$KEY_FROM_ENV" -eq 1 ]; then
    echo "    api_key left in the environment, where you put it"
elif [ -n "$API_KEY" ]; then
    set_config_key api_key "$API_KEY"
    echo "    api_key written to $CONFIG (0600)"
fi

# ---- microphone ------------------------------------------------------------
# The commonest way a working install still disappoints: the default source is
# a monitor or a jack with nothing in it, so every transcript comes back as
# caption boilerplate and the transcriber looks broken. Offer the list here,
# before the service starts, rather than leave it to the README.
if [ -z "$(config_value source)" ] && asking; then
    say "microphone"
    echo "    'source' is empty, so scribe will record from the system default."
    echo "    That is often not the microphone you speak into."
    echo
    if "$PREFIX/bin/scribe" --list-sources; then
        printf '    paste the source you want, or leave blank to keep the default: '
        read -r SOURCE || SOURCE=""
        if [ -n "$SOURCE" ]; then
            set_config_key source "$SOURCE"
            echo "    source = $SOURCE"
        else
            echo "    keeping the system default"
        fi
    else
        warn "could not list sources; set 'source' by hand later"
        warn "    scribe --list-sources"
    fi
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
HOTKEY="$(config_value hotkey)"
cat <<EOF
    scribe        the daemon (systemd user service)
    scribe-menu   settings and history panel

Hold ${HOTKEY:-your hotkey} and speak; the text lands in the focused window on release.

    scribe-menu               settings, level meter and history
    scribe --say "hello"      test injection without speaking
    scribe --list-sources     change microphone

Bind scribe-menu to a key in your compositor -- see menu/README.md.
EOF

if [ "$ENGINE" = openai ] && [ -z "$API_KEY" ] && [ -z "$(config_value api_key)" ]; then
    printf '\n\033[33m'
    echo "No API key is set, so the daemon will exit at startup."
    echo "Add one with scribe-menu, or put it in $CONFIG."
    printf '\033[0m'
fi

if [ "${NEED_LOGOUT:-0}" -eq 1 ]; then
    printf '\n\033[33m'
    echo "You must log out and back in before scribe can run."
    echo "Group membership is read when your session starts, so the service"
    echo "will fail on /dev/uinput until then. A new shell is not enough."
    printf '\033[0m'
fi
