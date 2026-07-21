# Scribe

Hold-to-talk voice transcription for Linux. Press and hold a key, speak, release —
the text lands in whatever window has focus.

The daemon holds no model and does no inference. It captures 16 kHz mono audio and
POSTs it to OpenAI's `/audio/transcriptions`. You supply an API key.

**Everything you dictate is uploaded to OpenAI.** That is the design: a thin client,
not a local transcriber. Do not dictate anything you would not paste into a web form.
Pointing `endpoint_url` at a local OpenAI-compatible server works — the request is the
same — but it is untested and unsupported.

## Status

| Piece | State |
|---|---|
| config, evdev hotkey, epoll input loop | done |
| persistent 16 kHz capture + gating | done |
| in-memory WAV + multipart POST + JSON parse | done |
| `wlr-vk` backend (Hyprland/Sway/river/Wayfire) | done |
| `clipboard` backend (universal fallback) | done |
| `uinput-layout` backend (GNOME/KDE direct typing) | done |
| `x11-xtest` backend (X11 direct typing) | not yet |

On GNOME/KDE Wayland, auto-detection lands on `uinput`.

## Install

Get an OpenAI API key first — [platform.openai.com/api-keys][keys]. scribe will not
start without one.

[keys]: https://platform.openai.com/api-keys

```sh
git clone https://github.com/ajr-khll/linux-transcription
cd linux-transcription
./install.sh
```

The script installs dependencies (dnf/apt/pacman), builds, installs to `/usr/local`,
adds you to the `input` group, seeds the config, asks for your key, and enables the
systemd user service. **Log out and back in afterwards** — group membership is read
when your session starts, so the service cannot open `/dev/uinput` until then.

Then:

```sh
scribe --list-sources     # pick a microphone, set `source` in the config
scribe --say "hello"      # test injection without speaking
scribe-menu               # settings and history panel
```

`./uninstall.sh` reverses it: stops the service, removes the binaries and the udev
rule, and leaves your config in place.

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
mkdir -p ~/.config/scribe && chmod 700 ~/.config/scribe
cp /usr/local/share/scribe/config.ini.example ~/.config/scribe/config.ini
chmod 600 ~/.config/scribe/config.ini  # it holds your API key
$EDITOR ~/.config/scribe/config.ini    # set api_key

systemctl --user daemon-reload
systemctl --user enable --now scribe
```

`make WITH_WLR_VK=0` drops the Wayland backend; `make WITH_MENU=0` drops the panel.

### The service does not start

The unit is `WantedBy=graphical-session.target`. GNOME and KDE activate that reliably.
Bare Hyprland, sway and river often never do, so `enable` links the unit and nothing
starts it. Check:

```sh
systemctl --user is-active graphical-session.target
```

If it prints `inactive`, either launch your compositor under `uwsm`, or start scribe
from the compositor's autostart — `exec-once = systemctl --user start scribe` on
Hyprland.

### Permissions

scribe reads `/dev/input/event*` and writes `/dev/uinput`. **Do not run it as root.**
Most distributions ship both as `root:input`, so joining the `input` group is enough.
Verify with `stat -c '%n %U:%G %a' /dev/uinput /dev/input/event0`; if the group is not
`input`, install `udev/99-scribe.rules` — see the comments in that file. (scribe
opens `/dev/uinput` write-only, so the common `0620` mode is fine.)

## Configure

`~/.config/scribe/config.ini`, mode `0600`. See `config.ini.example`.

`api_key` is the only required setting. `OPENAI_API_KEY` in the environment overrides
it — prefer that if you sync or back up your dotfiles, since a synced `0600` file is
still a copy of your key on someone else's disk.

`model` picks the transcription model: `whisper-1`, `gpt-4o-transcribe`, or
`gpt-4o-mini-transcribe`.

`audio_cues = on` (the default) plays a short tone so you can dictate without
watching the screen: a rising pair when the hotkey goes down, a falling pair
when it comes up, a low double beep when nothing usable came back. It uses
`paplay`, already pulled in with `pulseaudio-utils`.

## Settings panel

`scribe-menu` is the settings and transcript-history panel — a GJS + GTK4 app under
`menu/`. It does not link against the daemon: the two share only `config.ini`, a
`SIGHUP`, and the journal, so the daemon stays headless.

```sh
scribe-menu       # or launch "scribe" from your application menu
```

It enters your API key, picks the model, edits the config, and signals the daemon to
re-read it. A **level meter** on the selected source lets you pick a microphone by
watching the bar move rather than trusting a device name. With `history = on` it lists
past sessions.

See `menu/README.md` for dependencies, compositor rules and key bindings. Install
without it via `make WITH_MENU=0`.

Three settings have no panel yet — `backend`, `paste_chord`, and the injection test —
and are edited in `config.ini` directly. See `TODO.md`.

## Picking a microphone

`source` selects the capture device; empty means the system default, which is often
*not* the microphone you use.

```sh
scribe --list-sources          # samples each device and prints its peak level
```

If transcripts come back as plausible nonsense — `you`, `Thank you.`, `Subtitles by
the Amara.org community` — you are recording the wrong source. Whisper does not return
an empty string for audio with no voice in it; it emits caption boilerplate memorised
from training. scribe refuses two kinds of utterance before spending a request on them:

- **Silent.** Peaks below 2% of full scale. A live microphone reads well above that;
  an unplugged jack sits below it.
- **Flat.** Loud enough, but the level barely moves — hiss, a fan, an analog input
  with nothing plugged into it. Speech swings 24 dB or more between syllable and
  pause; the noise floors measured here swing 2 to 10. The cut is at 16 dB.

Both rejections log what they measured, so `journalctl --user -u scribe` says which
test failed and by how much.

The stream is pinned to the source you name, so the audio server cannot quietly move
scribe to another microphone mid-session. The cost of that: unplugging the configured
mic stops capture outright rather than migrating it. scribe says so in the log, and
picks the device up again on `systemctl --user reload scribe`.

## Run

```sh
build/scribe -v          # verbose
build/scribe -p          # print transcripts instead of injecting them
```

`-p` tests capture and inference without the injector.

## Backends

Auto-detection picks the first backend that works, preferring direct typing:

- **`wlr-vk`** — Wayland `zwp_virtual_keyboard_v1`. We upload our own keymap in which
  each needed character sits alone at level 1 of its own key, so the user's active
  layout is irrelevant. The manager global exists only on wlroots compositors, so the
  probe walks the registry rather than trusting `XDG_SESSION_TYPE`.
- **`uinput`** — direct typing where the virtual-keyboard protocol is absent (GNOME,
  KDE). uinput injects raw keycodes below the compositor, which reinterprets them
  through the active layout — so we ask libxkbcommon which keycode and level produces
  each character under the configured layout, and the two cancel. Requires `layout`
  (and `variant`) to match your **active** layout; if you hotkey-switch layouts, use
  `clipboard`. Characters no key can produce — curly quotes, em dashes — are dropped
  with a warning.
- **`clipboard`** — sets the clipboard via `wl-copy` (or `xclip` on X11) and emits a
  paste chord. Layout-agnostic, works everywhere, at the cost of clobbering the
  clipboard. Terminals usually need `paste_chord = ctrl+shift+v`.

Force one with `backend = wlr-vk | uinput | clipboard`.

### Known gaps

- `uinput` drops characters needing dead-key or compose sequences.
- `clipboard` overwrites the clipboard without restoring it.
- One `paste_chord` applies to every app, so a value tuned for terminals behaves oddly
  in GUI apps.

`TODO.md` has the full list.

## Design notes

- **No resampling.** The stream opens at exactly 16 kHz mono S16 — what Whisper wants.
- **Persistent capture** plus a 250 ms rolling pre-roll prepended to each utterance, so
  the first consonant survives the gap between keypress and the daemon noticing.
- **The input thread never blocks.** It flips an atomic and returns to `epoll`, so
  hotkey release is instant however slow transcription is.
- **No feedback loop.** The uinput device is created before enumeration with a known
  name, which the input thread skips.
- Utterances under 100 ms are dropped as stray taps; the queue is bounded and drops
  oldest, so a slow endpoint never stalls capture.

## License

scribe is licensed under the **GNU General Public License, version 3**. See `LICENSE`.
Source files carry an `SPDX-License-Identifier: GPL-3.0-only` header.

`protocol/virtual-keyboard-unstable-v1.xml` is vendored, not original. It is MIT
licensed (Kristian Høgsberg, Intel, Collabora, Purism) and keeps its notice inline; MIT
is GPL-compatible.

The libraries scribe links (libevdev, libpulse, libcurl, wayland-client, libxkbcommon)
are MIT or LGPL and compatible with GPLv3. One caveat: libcurl built against OpenSSL
1.x is *not* GPL-compatible; OpenSSL 3.x is Apache-2.0 and fine. A distribution shipping
libcurl on OpenSSL 1.x would need a linking exception.
