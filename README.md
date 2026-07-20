# whisprd

Hold-to-talk voice transcription for Linux. Press and hold a key, speak, release —
the text is typed into whatever window has focus.

The daemon holds no model and does no inference. It captures 16 kHz mono audio
and POSTs it to OpenAI's `/audio/transcriptions`. You supply an API key.

**Everything you dictate is uploaded to OpenAI.** That is the whole design: this
is a thin client, not a local transcriber. Do not dictate anything into it that
you would not paste into a web form. Running against a local OpenAI-compatible
server is possible — the request is identical either way — but it is untested
and unsupported here; see `endpoint_url` in `config.ini.example`.

## Status

| Piece | State |
|---|---|
| config, evdev hotkey, epoll input loop | implemented |
| persistent 16 kHz capture + gating | implemented |
| in-memory WAV + multipart POST + JSON parse | implemented |
| `wlr-vk` backend (Hyprland/Sway/river/Wayfire) | implemented |
| `clipboard` backend (universal fallback) | implemented |
| `uinput-layout` backend (GNOME/KDE direct typing) | implemented |
| `x11-xtest` backend (X11 direct typing) | not yet |

On GNOME/KDE Wayland, auto-detection lands on `uinput`.

## Install

You need an OpenAI API key first — [platform.openai.com/api-keys][keys].
whisprd will not start without one.

[keys]: https://platform.openai.com/api-keys

```sh
git clone https://github.com/ajr-khll/linux-transcription
cd linux-transcription
./install.sh
```

The script installs dependencies (dnf/apt/pacman), builds, installs to
`/usr/local`, adds you to the `input` group, seeds the config, asks for your
key, and enables the systemd user service. **Log out and back in afterwards** —
group membership is read when your session starts, so the service cannot open
`/dev/uinput` until then.

Then:

```sh
whisprd --list-sources     # pick a microphone, set `source` in the config
whisprd --say "hello"      # test injection without speaking
whisprd-menu               # settings and history panel
```

### By hand

```sh
# Fedora
sudo dnf install libevdev-devel pulseaudio-libs-devel libcurl-devel \
                 libxkbcommon-devel wayland-devel wayland-protocols-devel \
                 gjs gtk4 pulseaudio-utils ibm-plex-mono-fonts
# Debian/Ubuntu
sudo apt install libevdev-dev libpulse-dev libcurl4-openssl-dev \
                 libxkbcommon-dev libwayland-dev wayland-protocols \
                 gjs libgtk-4-1 pulseaudio-utils fonts-ibm-plex

make && make test
sudo make install

sudo usermod -aG input $USER            # then log out and back in
mkdir -p ~/.config/whisprd && chmod 700 ~/.config/whisprd
cp /usr/local/share/whisprd/config.ini.example ~/.config/whisprd/config.ini
chmod 600 ~/.config/whisprd/config.ini  # it holds your API key
$EDITOR ~/.config/whisprd/config.ini    # set api_key

systemctl --user daemon-reload
systemctl --user enable --now whisprd
```

`make WITH_WLR_VK=0` drops the Wayland backend; `make WITH_MENU=0` drops the panel.

### The service does not start

The unit is `WantedBy=graphical-session.target`. GNOME and KDE activate that
reliably. Bare Hyprland, sway and river often never do, so `enable` links the
unit and nothing ever starts it. Check with:

```sh
systemctl --user is-active graphical-session.target
```

If that prints `inactive`, either launch your compositor under `uwsm`, or start
whisprd from the compositor's own autostart —
`exec-once = systemctl --user start whisprd` on Hyprland.

### Permissions

whisprd reads `/dev/input/event*` and writes `/dev/uinput`. **Do not run it as
root.** Most distributions ship both as `root:input`, so joining the `input`
group is enough. Verify with
`stat -c '%n %U:%G %a' /dev/uinput /dev/input/event0`; if the group is not
`input`, install `udev/99-whisprd.rules` — see the comments in that file.
(whisprd opens `/dev/uinput` write-only, so the common `0620` mode is fine.)

## Configure

`~/.config/whisprd/config.ini`, mode `0600`. See `config.ini.example`.

`api_key` is the only setting that is required. `OPENAI_API_KEY` in the
environment overrides it — prefer that if you sync or back up your dotfiles,
since a synced `0600` file is still a copy of your key on someone else's disk.

## Settings panel

`whisprd-menu` is the settings and transcript-history panel — a GJS + GTK4
application under `menu/`. It does not compile and does not link against the
daemon: the two share only `config.ini`, a `SIGHUP`, and the journal, so the
daemon keeps no GUI dependencies and still runs headless.

```sh
whisprd-menu       # or launch "whisprd" from your application menu
```

It edits `~/.config/whisprd/config.ini`, signals the daemon to re-read it, and
shows a **level meter** on the selected capture source so you pick a microphone
by watching the bar move rather than trusting a device name. With `history = on`
it also lists past sessions.

See `menu/README.md` for dependencies, compositor rules and key bindings.
Install without it via `make WITH_MENU=0`.

Three settings have no panel yet — `backend`, `paste_chord`, and the injection
test — and are edited in `config.ini` directly. See `TODO.md`.

## Picking a microphone

`source` selects the capture device; empty means the system default, which is
often *not* the microphone you actually use.

```sh
whisprd --list-sources          # samples each device and prints its peak level
```

If transcripts come back as plausible-looking nonsense — `you`, `Thank you.`,
`Subtitles by the Amara.org community`, a stray copyright line — you are almost
certainly recording silence from the wrong source. Whisper does not return an
empty string for silent audio; it emits caption boilerplate memorised from its
training data. whisprd guards against this by refusing to transcribe any
utterance peaking below 2% of full scale, and tells you the measured level.

A live microphone reads well above 2% of full scale; an unplugged jack sits
below it, which is the noise floor rather than audio.

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
- **`uinput`** — direct typing where the virtual-keyboard protocol does not exist
  (GNOME, KDE). uinput injects raw keycodes *below* the compositor, which then
  interprets them through the user's active layout — so we ask libxkbcommon which
  keycode and modifier level produces each character under the layout declared in
  config, and emit that. The compositor applies the same layout on the way out and
  the two cancel. Requires `layout` (and `variant`, if you use one) to match your
  **active** layout; if you hotkey-switch layouts, use `clipboard` instead.
  Characters no key can produce — curly quotes and em dashes, which Whisper does
  emit — are dropped with a warning, since reaching them needs dead-key/compose
  sequences this backend does not implement.
- **`clipboard`** — sets the clipboard via `wl-copy` (or `xclip` on X11) and emits a
  paste chord through uinput. Layout-agnostic, works everywhere, at the cost of
  clobbering the clipboard. Terminals usually need `paste_chord = ctrl+shift+v`.

Force one with `backend = wlr-vk | uinput | clipboard` in the config.

### Known gaps

- The `uinput` backend drops characters that need dead-key or compose sequences.
- The clipboard backend overwrites the clipboard without restoring it.
- A single `paste_chord` applies to every app, so a config tuned for terminals
  behaves oddly in GUI apps and vice versa.

`TODO.md` has the full list, including what is missing before a release.

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

## License

whisprd is licensed under the **GNU General Public License, version 3**. See
`LICENSE` for the full text. Source files carry an `SPDX-License-Identifier:
GPL-3.0-only` header.

`protocol/virtual-keyboard-unstable-v1.xml` is vendored, not original to this
project. It is MIT licensed (Kristian Høgsberg, Intel, Collabora, Purism) and
retains its own copyright notice inline; MIT is GPL-compatible, so it may be
redistributed as part of this tree.

The libraries whisprd links (libevdev, libpulse, libcurl, wayland-client,
libxkbcommon) are MIT or LGPL and compatible with GPLv3. One caveat worth
knowing: libcurl is commonly built against OpenSSL, and OpenSSL 1.x's license is
*not* GPL-compatible. OpenSSL 3.x is Apache-2.0 and is fine. Distributions still
shipping libcurl linked to OpenSSL 1.x would need a linking exception.
