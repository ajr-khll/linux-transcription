# TODO

Known gaps, in rough order of how much they matter. Things the code does not
do and does not pretend to do; the READMEs describe what exists.

## The speech detector's thresholds are compile-time constants

`VAD_SILENCE_PEAK` and `VAD_MIN_SPREAD_DB` in `src/vad.h` were measured on one
machine: a USB camera mic against an onboard analog input with nothing plugged
into it. The numbers that came out were 2.2 dB of spread for the idle USB mic,
10.4 dB for the dead jack, and 24-30 dB for speech, so the threshold sits at 16.

None of that is guaranteed to transfer. A noisier room, a quieter speaker, a
gain-staged interface or a mic with AGC all move both numbers, and the failure
is not symmetrical: too low wastes an API call on a nonsense transcript, too
high silently eats what the user said. Right now the only fix is editing a
header and rebuilding, which is not a fix a user has.

- Both should be config keys, read at `audio_init` alongside `source`.
- `scribe --list-sources` already samples every device; it could print the
  spread next to the peak and suggest a threshold, so picking a value is a
  reading exercise rather than a guess.
- `test_vad <file.wav>` prints what a recording measures and is the tuning tool
  today. Whatever ships should point at it, or absorb it.
- The menu should expose them too, though probably as one "how strict" control
  rather than two decibel figures.

## Hallucinated transcripts are only half-fixed

The envelope test in `src/vad.c` removes the common cause: a dead or wrong
capture source, which Whisper answers with caption boilerplate rather than an
empty string. That is an input-side guard and it is all we have. Everything
below is still open.

- **It still happens on real speech.** Short, quiet, clipped or half-spoken
  audio produces the same boilerplate, and by definition the envelope test
  passes it -- there is speech in there. The guard cannot see this class at
  all.
- **Nothing checks the output.** Every transcript is trusted once it comes
  back. The daemon has no notion of a suspicious result.
- **We throw away Whisper's own confidence.** `transcribe.c` asks for
  `response_format=json`. `verbose_json` returns `no_speech_prob` and
  `avg_logprob` per segment, and a high `no_speech_prob` alongside fluent text
  is precisely the hallucination signature. We never see it because we never
  ask for it. This is the cheapest real improvement available.
- **`language` is never sent**, so every request auto-detects. On marginal
  audio it picks a language and then produces confident boilerplate in it.
  The config already knows the keyboard layout; the spoken language is a
  reasonable thing to ask for too.
- **`temperature` is never sent.** The API falls back to higher temperatures
  on low-confidence decodes, which is exactly when boilerplate appears.
  Pinning it to 0 trades a little accuracy on hard audio for less invention.
- **A phrase blocklist** ("Thank you.", "Subtitles by the Amara.org
  community", a bare "you") is the obvious fix and the fragile one: it is
  locale-dependent, it rots as models change, and it will eventually eat a
  real transcript. If it goes in it belongs behind a confidence check, not
  instead of one.
- **None of this is measured.** There is no record of how often it still
  happens or on what. `history = on` keeps transcripts but nothing keeps the
  audio that produced them, so a bad result cannot be replayed. Some way to
  retain the PCM behind a suspicious transcript would make the rest of this
  list tractable instead of guesswork.

## Settings the menu cannot reach

`scribe-menu` replaced the old GTK4 settings app, and three settings lost
their UI in the move. All three still work — the daemon reads them from
`config.ini`, and `menu/src/config.js` keeps them in `KEYS[]` so the menu
round-trips them untouched rather than dropping them on save. They just have
no panel, and the defaults are what most people want.

- **`backend`** — `auto` picks the first backend that probes clean, which is
  right almost everywhere. Forcing one needs a hand edit. A dropdown belongs
  in an `─ injection ─` field beside the layout selector.
- **`paste_chord`** — only consulted by the `clipboard` backend, and only
  wrong in terminals, which usually want `ctrl+shift+v`. Same field.
- **Test injection** — `scribe --say TEXT` confirms the backend works without
  speaking. The old GUI had a button for it; the CLI flag is unchanged. Worth
  a `[ test ]` button next to the backend dropdown.

## The systemd unit and install.sh

- `install.sh` covers dnf, apt and pacman. Anything else falls through with a
  warning and relies on the build to name what is missing.
- The unit is `WantedBy=graphical-session.target`, which GNOME and KDE reach
  and bare Hyprland/sway/river frequently do not — `enable` then links a unit
  nothing ever starts. `install.sh` detects this and prints the fix rather than
  rewriting the unit, since `default.target` would start scribe on TTY logins
  too. A `uwsm`-aware unit, or a documented `exec-once`, is the real answer.

## Cost and failure, now that every utterance is a paid API call

Going OpenAI-only turned three tolerable behaviours into ones that cost money
or lose work.

- **No spend limit.** Nothing caps requests. A stuck hotkey, or a hotkey bound
  to a key you actually type, uploads audio continuously and bills for it. The
  100 ms minimum, the 2% silence guard and the envelope test help, but none of
  them is a budget. A daily request cap in the config would be a small change.
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
  scribe runs outside systemd.

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
