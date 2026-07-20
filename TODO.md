# TODO

Known gaps, in rough order of how much they matter. Things the code does not
do and does not pretend to do; the READMEs describe what exists.

## Settings the menu cannot reach

`whisprd-menu` replaced the old GTK4 `whisprd-gui`, and three settings lost
their UI in the move. All three still work — the daemon reads them from
`config.ini`, and `menu/src/config.js` keeps them in `KEYS[]` so the menu
round-trips them untouched rather than dropping them on save. They just have
no panel, and the defaults are what most people want.

- **`backend`** — `auto` picks the first backend that probes clean, which is
  right almost everywhere. Forcing one needs a hand edit. A dropdown belongs
  in an `─ injection ─` field beside the layout selector.
- **`paste_chord`** — only consulted by the `clipboard` backend, and only
  wrong in terminals, which usually want `ctrl+shift+v`. Same field.
- **Test injection** — `whisprd --say TEXT` confirms the backend works without
  speaking. The old GUI had a button for it; the CLI flag is unchanged. Worth
  a `[ test ]` button next to the backend dropdown.

## The systemd unit and install.sh

- `install.sh` covers dnf, apt and pacman. Anything else falls through with a
  warning and relies on the build to name what is missing.
- The unit is `WantedBy=graphical-session.target`, which GNOME and KDE reach
  and bare Hyprland/sway/river frequently do not — `enable` then links a unit
  nothing ever starts. `install.sh` detects this and prints the fix rather than
  rewriting the unit, since `default.target` would start whisprd on TTY logins
  too. A `uwsm`-aware unit, or a documented `exec-once`, is the real answer.

## Cost and failure, now that every utterance is a paid API call

Going OpenAI-only turned three tolerable behaviours into ones that cost money
or lose work.

- **No spend limit.** Nothing caps requests. A stuck hotkey, or a hotkey bound
  to a key you actually type, uploads audio continuously and bills for it. The
  100 ms minimum and the 2% silence guard help, but neither is a budget. A
  daily request cap in the config would be a small change.
- **A failed request loses the transcript.** `transcribe_pcm` logs the error
  and returns NULL; the worker drops it. On a local server that meant a lost
  utterance on a machine you controlled. Against a network endpoint it means
  every flaky connection silently eats what you said. Retry twice with backoff
  before giving up.
- **The queue drops oldest at 4.** Fine when inference was local and fast.
  A slow round-trip to OpenAI makes overflow much more likely, and the thing
  discarded is speech the user has already produced.
- **The key is logged nowhere, but it is in argv range.** `--say` and the
  config path are fine, but anyone adding a debug print of the config struct
  would leak the key into the journal. Worth a comment in `config.h`.

## Injection

- The `uinput` backend drops characters needing dead-key or compose sequences.
  Whisper emits curly quotes and em dashes regularly, so this fires often.
  `tests/test_layout.c` pins the current behaviour.
- The `clipboard` backend overwrites the clipboard and never restores it.
- One `paste_chord` applies to every app, so a value tuned for terminals
  misbehaves in GUI apps and the reverse.
- No `x11-xtest` backend. X11 sessions fall through to `clipboard`.

## Daemon/menu coupling

The daemon exposes no socket and no D-Bus interface, so the menu reaches it
three ways, all textual and none checked at build time:

- **`config.ini`** is parsed twice, by `src/config.c` and again by
  `menu/src/config.js`. `KEYS[]` in the latter is a hand-kept mirror of the
  former. Add a key to the daemon without adding it there and the menu will
  silently drop it on the next save. Nothing catches this.
- **The live feed** greps `journalctl` for the `transcript:` prefix written at
  `src/main.c:59`. Changing that string breaks the feed with no error.
- **Status** is `systemctl --user is-active`, so it reads inactive whenever
  whisprd runs outside systemd.

A real IPC surface would remove all three. That is a pre-1.0 decision, not a
packaging one.

## Packaging

- No RPM spec, PKGBUILD or debian/. The split is one arch package (daemon)
  plus one noarch (menu), and the menu must pin `= %{version}-%{release}`:
  the coupling above is textual, so a mismatched pair fails silently instead
  of failing to load.
- The menu's dependencies are all programs it shells out to, never linked, so
  automatic dependency generators find none of them. Declare `gjs`, `gtk4`,
  `pulseaudio-utils`, `systemd` by hand; `gtk4-layer-shell` is `Recommends`.
- The menu carries no version. `VERSION` in the top-level Makefile is the only
  one, and nothing propagates it.
- No man page, no CHANGELOG, no release tag.
- No CI. `make test` needs neither network nor compositor and nothing runs it.
- `menu_handoff/` is a design bundle, not build input. It should not ship.

## Smaller

- No icon of its own; the desktop entry borrows `audio-input-microphone`.
