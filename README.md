# Scribe

Hold-to-talk voice transcription for Linux. Press and hold a key, speak, release —
the text lands in whatever window has focus.

With the local engine it can optionally show the words *while* you speak, correcting
them to the final transcript when you let go. Off by default — see
[Configuration](#configuration).

It captures 16 kHz mono audio and transcribes it one of two ways, set by `engine`
in the config:

- **`parakeet`** — NVIDIA Parakeet TDT 0.6B v3, decoded in the daemon's own
  process. No account, no key, no bill, no network. **Nothing you say leaves the
  machine.**
- **`openai`** — a multipart POST to OpenAI's `/audio/transcriptions`. You supply
  an API key, and **everything you dictate is uploaded.**

`./install.sh` offers the local engine first and takes it if you just press return.
`config.ini.example` ships `engine = openai` instead, because a plain `make` has no
local engine compiled in — so that is the right default for a hand-built install and
for every install that predates Parakeet. Reach for `openai` when you need one of the
~74 languages Parakeet does not cover.

## Status

| Piece | State |
|---|---|
| config, evdev hotkey, epoll input loop | done |
| persistent 16 kHz capture + gating | done |
| in-memory WAV + multipart POST + JSON parse | done |
| on-device Parakeet via sherpa-onnx (`WITH_PARAKEET=1`) | done |
| live preview, corrected on release (`live = on`, opt-in) | done |
| semantic cleanup via a local LLM (`cleanup = on`, opt-in) | done |
| `wlr-vk` backend (Hyprland/Sway/river/Wayfire) | done |
| `clipboard` backend (universal fallback) | done |
| `uinput-layout` backend (GNOME/KDE direct typing) | done |
| `x11-xtest` backend (X11 direct typing) | not yet |

On GNOME/KDE Wayland, auto-detection lands on `uinput`.

## Install

```sh
git clone https://github.com/ajr-khll/linux-transcription
cd linux-transcription
./install.sh
```

Every question comes first — which engine, your API key if you picked cloud — then it
prints what it is about to do, takes your password once, and runs unattended:
dependencies (dnf/apt/pacman), build, install to `/usr/local`, the `input` group, your
config, the systemd user service. At the end it lists your capture devices with live
levels so you can pick a microphone, which is the one setting a fresh install most
often gets wrong. **Log out and back in afterwards** — group membership is read when
your session starts, so the service cannot open `/dev/uinput` until then.

Answers already in your config are kept, so re-running it is safe.

Every question can be answered on the command line instead — `./install.sh --help`:

```sh
./install.sh --engine=local                      # no questions worth asking
./install.sh --engine=cloud --api-key=sk-...     # or set OPENAI_API_KEY
./install.sh -y                                  # defaults, ask nothing
./install.sh --skip-deps                         # deps already handled
```

`OPENAI_API_KEY` in the environment is used but never copied into the config file.

`--prefix` takes `/usr/local` (the default) or `/usr`, and refuses anything else.
systemd looks for user units in a fixed set of directories, so a unit installed to
`/opt/scribe/lib/systemd/user` or `~/.local/lib/systemd/user` is never found — the
install would report success and the service would then not exist. To put scribe
somewhere else, follow [By hand](#by-hand) and start it yourself.

Then:

```sh
scribe --list-sources     # change microphone; sets `source` in the config
scribe --say "hello"      # test injection without speaking
scribe-menu               # settings and history panel
```

[keys]: https://platform.openai.com/api-keys

`./uninstall.sh` reverses it: stops the service, removes the binaries, the udev rule
and any downloaded model — naming the model directory and its size first, since that
is most of a gigabyte going without being asked about. Your own data is asked about,
in two separate questions: the config (which holds your API key), and the transcripts
under `~/.local/share/scribe/transcriptions`, with a file count and size. Both default
to keeping. It does not remove you from the `input` group, and says so.

### By hand

```sh
# Fedora
sudo dnf install gcc make pkgconf-pkg-config curl bzip2 \
                 libevdev-devel pulseaudio-libs-devel libcurl-devel \
                 libxkbcommon-devel wayland-devel wayland-protocols-devel \
                 gjs gtk4 pulseaudio-utils ibm-plex-mono-fonts
# Debian/Ubuntu
sudo apt install build-essential pkg-config curl bzip2 \
                 libevdev-dev libpulse-dev libcurl4-openssl-dev \
                 libxkbcommon-dev libwayland-dev wayland-protocols \
                 gjs libgtk-4-1 gir1.2-gtk-4.0 pulseaudio-utils fonts-ibm-plex
# Arch
sudo pacman -S --needed base-devel libevdev libpulse curl bzip2 libxkbcommon \
                        wayland wayland-protocols gjs gtk4 ttf-ibm-plex

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

Two of those are easy to miss. On Debian, `gir1.2-gtk-4.0` is a separate package from
`libgtk-4-1`, and `scribe-menu` reaches GTK through GObject introspection — without the
typelib the panel dies on its first import while the daemon looks perfectly healthy.
`curl` and `bzip2` are for `install-parakeet.sh`; GNU `tar` execs `bzip2` rather than
decompressing `.tar.bz2` itself, so a missing one fails *after* the 490 MB download.

`make WITH_WLR_VK=0` drops the Wayland backend; `make WITH_MENU=0` drops the panel;
`make WITH_PARAKEET=1` adds the local engine (see below).

## Local transcription

```sh
./install-parakeet.sh          # ~560 MB, ~710 MB on disk
make WITH_PARAKEET=1 && sudo make install
```

The flag is needed once, not on every command: it is recorded in
`build/config.mk` and reused by every later `make` in that tree, so
`sudo make install` installs the engine you just built rather than quietly
rebuilding without it. `make WITH_PARAKEET=0` turns it back off; `make clean`
forgets the setting.

Then set `engine = parakeet` in `~/.config/scribe/config.ini` and
`systemctl --user reload scribe`. `./install.sh` does all of this for you if you
pick local when it asks.

What it costs and what it buys:

| | |
|---|---|
| Download | ~560 MB (a 25 MB runtime, the model, and a 70 MB model for the live preview) |
| Disk | ~710 MB under `/usr/local/share/scribe/models` |
| Memory | ~1 GB resident while the daemon runs, loaded at start and kept |
| Startup | a second or two to load the model; logged, so you can see why |
| Languages | 25 European. For anything else, use `engine = openai` |
| Network | none. Nothing you dictate leaves the machine |
| Cost | none |

The model loads once and stays loaded: a config reload — which now happens on every
save of `config.ini` — does *not* reload it unless `model_dir` or `threads` changed.

`threads` sets the decode thread count (`0` means 4). `model_dir` points at the model
if you put it somewhere other than the default.

Check an install without a microphone:

```sh
make test-parakeet
build/test_parakeet /usr/local/share/scribe/models/parakeet-tdt-0.6b-v3-int8/test.wav
```

There is a matching check for the live preview's model, which also reports how far
behind real time it runs and how often it revises words it has already emitted:

```sh
make test-stream
build/test_stream /usr/local/share/scribe/models/streaming-zipformer-en/test.wav
```

That loads the model, decodes the sample shipped with it, and checks that a second
load reuses the first. It is kept out of `make test` because a plain checkout has no
model to give it.

Under the hood this is [sherpa-onnx][sherpa] (Apache-2.0), pinned to v1.13.4 and
installed to `/usr/local/lib/scribe`. It bundles its own ONNX Runtime, so there is no
system-wide dependency to hunt down and nothing is added to `ldconfig` — the daemon
finds it by rpath.

[sherpa]: https://github.com/k2-fsa/sherpa-onnx

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

## Cleaning up transcripts

Speech is not writing. You say *"let's meet at 5pm on Monday, uh, no 5pm on tuesday"*
and mean *"Let's meet at 5pm on Tuesday."* With `cleanup = on`, scribe passes each
transcript through a local instruction model that drops fillers, resolves a spoken
self-correction, and adds punctuation — without rephrasing. The words stay yours.

The raw transcript is typed first; when the model answers, scribe backspaces over it and
types the corrected version — the same swap the live preview uses, so it needs a
backend that can erase (`wlr-vk` or `uinput`, not `clipboard`). If the model
does not answer within `cleanup_timeout_ms`, the raw transcript stands. Nothing is ever
held back waiting on it.

**Local only.** The endpoint must be on this machine — scribe refuses to start if
`cleanup_endpoint_url` points anywhere else — so turning cleanup on cannot quietly start
uploading everything you dictate. Any OpenAI-compatible chat server works; the default
is [Ollama][ollama]'s port:

```sh
ollama serve
ollama pull qwen2.5:3b-instruct
```

then set `cleanup = on` and `cleanup_model = qwen2.5:3b-instruct`. A `vocabulary_file`
(default `~/.config/scribe/vocabulary.txt`, one term per line) lists names and jargon
the model should spell correctly.

[ollama]: https://ollama.com

## Configure

`~/.config/scribe/config.ini`, mode `0600`. See `config.ini.example`.

**Edits apply on save.** scribe watches the file and reloads itself, so there is no
`systemctl --user reload` to remember. It tears the stack down and stands it back up
in the same process — same PID, same virtual keyboard — and keeps the local models
loaded unless `model_dir`, `live_model_dir` or `threads` changed. `SIGHUP` still does
the same thing if you want to trigger it by hand.

The watch is on the directory rather than the file, because saving by writing a temp
file and renaming over the target — what `scribe-menu` and every careful editor do —
replaces the inode. A watch on the file would fire once, for the first save, and be
silently dead after that.

A reload discards any utterance in flight, so avoid saving mid-sentence.

`engine` picks the transcriber: `openai` (the default) or `parakeet`.

With `engine = openai`, `api_key` is required. `OPENAI_API_KEY` in the environment
overrides it — prefer that if you sync or back up your dotfiles, since a synced `0600`
file is still a copy of your key on someone else's disk. `model` picks the model:
`whisper-1`, `gpt-4o-transcribe`, or `gpt-4o-mini-transcribe`.

With `engine = parakeet`, none of those are read. `model_dir` and `threads` apply
instead — see [Local transcription](#local-transcription).

`live = on` (**off** by default) types words into the focused window as you speak
them instead of making you wait for the whole utterance. What you see while holding
the key is a guess from a small streaming model; when you let go, scribe backspaces
over it and types what Parakeet actually heard. The transcript is identical either
way — this only changes when you see it.

It is opt-in because it is rough. The preview has no punctuation, gets words wrong,
and corrects itself in your own document while you watch. Some people like seeing the
machine keep up; others would rather the text arrive once and be right.

Parakeet cannot do this itself. It is an offline model with no streaming export, so
the preview needs a second, smaller model, which `install-parakeet.sh` fetches. The
preview is therefore rougher than the final text: expect missing punctuation and the
occasional wrong word, both corrected on release.

**Your hotkey must not be a modifier.** The preview types while you are still
holding the key, so with the default `KEY_RIGHTCTRL` every letter it produced would
arrive as a shortcut in the focused window — `Ctrl+V` pastes, `Ctrl+W` closes it,
`Ctrl+Q` quits it. Zeroing the virtual keyboard's own modifiers does not help: your
physical Ctrl really is down and the compositor is right to say so. Batch injection
never met this because it types after the release. Pick a spare key instead:

```ini
hotkey = KEY_F13        # or KEY_SCROLLLOCK, or anything you do not otherwise type
```

Live mode runs only where all four of these hold, and the log says which one failed:

- a hotkey that holds no modifier down, as above.
- `engine = parakeet`. There is no live path to a network endpoint.
- an injection backend that can erase — `wlr-vk` or `uinput`. The `clipboard`
  backend pastes rather than types, so nothing it puts on screen can be taken back.
- the streaming model installed.

Anywhere else scribe silently does what it always did and types the transcript in one
go. `live_model_dir` points at the model if you put it somewhere unusual.

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

Some settings have no panel yet — `backend`, `paste_chord`, the injection test, and
the engine keys — and are edited in `config.ini` directly. See `TODO.md`.

## Picking a microphone

`source` selects the capture device; empty means the system default, which is often
*not* the microphone you use.

```sh
scribe --list-sources          # samples each device and prints its peak level
```

If transcripts come back as plausible nonsense — `you`, `Thank you.`, `Subtitles by
the Amara.org community` — you are recording the wrong source. Whisper does not return
an empty string for audio with no voice in it; it emits caption boilerplate memorised
from training. scribe refuses two kinds of utterance before transcribing them at all:

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
- With `cleanup = on`, if you move the caret or type in the moment between the raw
  transcript and its correction, the backspaces land in the wrong place. The timeout is
  short to keep that window small.

`TODO.md` has the full list.

## Design notes

- **No resampling.** The stream opens at exactly 16 kHz mono S16 — what Whisper wants,
  and what Parakeet wants too.
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

Built with `WITH_PARAKEET=1`, scribe links **sherpa-onnx** (Apache-2.0) and the ONNX
Runtime it bundles (MIT). Both are GPLv3-compatible. Neither is vendored here —
`install-parakeet.sh` downloads the upstream release.

The transcriber is **NVIDIA Parakeet TDT 0.6B v3**, licensed
[CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/). It is downloaded, not
shipped, and it is data rather than a linked work — but if you redistribute it,
attribute NVIDIA.

The live preview uses a **k2-fsa streaming zipformer**
(`sherpa-onnx-streaming-zipformer-en-2023-06-26`), Apache-2.0, on the same terms:
downloaded by `install-parakeet.sh`, not shipped here.

The libraries scribe links (libevdev, libpulse, libcurl, wayland-client, libxkbcommon)
are MIT or LGPL and compatible with GPLv3. One caveat: libcurl built against OpenSSL
1.x is *not* GPL-compatible; OpenSSL 3.x is Apache-2.0 and fine. A distribution shipping
libcurl on OpenSSL 1.x would need a linking exception.
