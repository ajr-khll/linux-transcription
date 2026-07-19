# whisprd

Hold-to-talk voice transcription for Linux. Press and hold a key, speak, release —
the text is typed into whatever window has focus.

The daemon holds no model and does no inference. It captures 16 kHz mono audio and
POSTs it to an OpenAI-compatible `/audio/transcriptions` endpoint. Whether that
endpoint is a local `whisper-server` on `127.0.0.1` or the OpenAI API is a single
config value — the code path is identical, and in local mode nothing leaves the
machine.

## Status

| Piece | State |
|---|---|
| config, evdev hotkey, epoll input loop | implemented |
| persistent 16 kHz capture + gating | implemented |
| in-memory WAV + multipart POST + JSON parse | implemented |
| `wlr-vk` backend (Hyprland/Sway/river/Wayfire) | implemented |
| `clipboard` backend (universal fallback) | implemented |
| `uinput-layout` backend (GNOME/KDE direct typing) | not yet |
| `x11-xtest` backend (X11 direct typing) | not yet |

On GNOME/KDE Wayland, auto-detection currently lands on `clipboard`.

## Build

```sh
# Fedora
sudo dnf install libevdev-devel pulseaudio-libs-devel libcurl-devel \
                 libxkbcommon-devel wayland-devel wayland-protocols-devel
# Debian/Ubuntu
sudo apt install libevdev-dev libpulse-dev libcurl4-openssl-dev \
                 libxkbcommon-dev libwayland-dev

make
make test          # JSON + keymap round-trip tests; no network, no compositor
```

Build without the Wayland backend with `make WITH_WLR_VK=0`.

## Permissions

whisprd reads `/dev/input/event*` and writes `/dev/uinput`. **Do not run it as root.**

```sh
sudo cp udev/99-whisprd.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
sudo usermod -aG input $USER
```

Log out and back in for the group change to apply.

## Configure

```sh
mkdir -p ~/.config/whisprd
cp config.ini.example ~/.config/whisprd/config.ini
```

See `config.ini.example`. The only setting that decides local vs cloud is
`endpoint_url`; `api_key` is needed for the cloud and ignored locally.

## Run

```sh
build/whisprd -v          # verbose
build/whisprd -p          # print transcripts instead of injecting them
```

`-p` is the way to test capture and inference without involving the injector at all.

## Backends

Auto-detection picks the first backend that works, preferring direct typing:

- **`wlr-vk`** — Wayland `zwp_virtual_keyboard_v1`. We upload our *own* keymap in
  which each needed character sits alone at level 1 of its own key, so the user's
  active layout is irrelevant and no modifier handling is needed. The manager global
  exists only on wlroots compositors and binds silently-absent elsewhere, so the
  probe walks the registry rather than trusting `XDG_SESSION_TYPE`.
- **`clipboard`** — sets the clipboard via `wl-copy` (or `xclip` on X11) and emits a
  paste chord through uinput. Layout-agnostic, works everywhere, at the cost of
  clobbering the clipboard. Terminals usually need `paste_chord = ctrl+shift+v`.

Force one with `backend = wlr-vk | clipboard` in the config.

### Known gaps

- The clipboard backend overwrites the clipboard without restoring it.
- A single `paste_chord` applies to every app, so a config tuned for terminals
  behaves oddly in GUI apps and vice versa.

## Design notes

- **No resampling anywhere.** The capture stream is opened at exactly 16 kHz mono
  S16, which is what Whisper wants.
- **Persistent capture stream** plus a 250 ms rolling pre-roll that is prepended to
  each utterance, so the first consonant survives the gap between the physical
  keypress and the daemon observing it.
- **The input thread never blocks.** It only flips an atomic and returns to `epoll`,
  so hotkey release is instant regardless of how slow transcription is.
- **No feedback loop.** The uinput device is created before device enumeration and
  carries a known name, which the input thread skips.
- Utterances shorter than 100 ms are discarded as stray taps; the queue is bounded
  and drops oldest, so a slow endpoint can never stall capture.
