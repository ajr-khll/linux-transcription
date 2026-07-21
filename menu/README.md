# whisprd-menu

Floating settings + history panel for the whisprd daemon — design `4b`, built
on GJS + GTK4.

```
cd menu
make deps      # check runtime dependencies
make run       # open the panel
```

`Esc` or `^X` closes it.

## Dependencies

The panel links against none of these — it shells out to all of them. Nothing
here is discovered automatically, so a package has to name every one by hand.

| | |
|---|---|
| `gjs`, `gtk4` | GTK4 JavaScript runtime |
| `pactl`, `parec` | capture device names, level meter |
| `journalctl`, `systemctl` | live feed, status, config reload |
| `gtk4-layer-shell` | optional; only for `--layer` mode |
| **IBM Plex Mono** | `sudo dnf install ibm-plex-mono-fonts` |

Without the font the layout is correct but rendered in the wrong typeface.

**Astal is not required.** `app.js` imports none of it, and `src/audio.js` treats
`AstalWp` as an optional fallback for device names. This matters for packaging:
the `astal*` packages live only in the `solopasha/hyprland` COPR, so depending
on them would keep whisprd out of any ordinary repository.

> Fedora's `dnf install ags` is **Adventure Game Studio**, an unrelated project
> that happens to share the name.

## Compositor setup

The panel is a normal toplevel: movable, focusable, alt-tabbable, and
targetable by window rules. It identifies as:

```
app_id / class : dev.whisprd.Menu
title          : whisprd
size           : 940x566, non-resizable
```

Because it is undecorated, there is no titlebar to drag by — instead the whole
panel is a `Gtk.WindowHandle`, so dragging any part that is not itself a
control moves the window.

### Hyprland

```
windowrulev2 = float,        class:^(dev\.whisprd\.Menu)$
windowrulev2 = size 940 566, class:^(dev\.whisprd\.Menu)$
windowrulev2 = center,       class:^(dev\.whisprd\.Menu)$
bind = SUPER, W, exec, /usr/local/bin/whisprd-menu
```

### Sway

```
for_window [app_id="dev.whisprd.Menu"] floating enable, resize set 940 566, move position center
bindsym $mod+w exec /usr/local/bin/whisprd-menu
```

### i3 (X11)

```
for_window [class="dev.whisprd.Menu"] floating enable, resize set 940 566, move position center
bindsym $mod+w exec /usr/local/bin/whisprd-menu
```

Hyprland already floats it without a rule, since it is fixed-size — the rules
above just make the placement explicit.

The `application_id` gives it single-instance behaviour: pressing the keybind
while the panel is already open focuses it rather than starting a second copy.

### Overlay mode

`whisprd-menu --layer` (or `SCRIBE_MENU_LAYER=1`) makes it a layer-shell
surface instead: always centred with no window rules, drawn above everything.
The trade is that a layer surface is not a window — the compositor gives it no
move, no ordinary focus and no alt-tab, which is why it is not the default.

The `whisprd-menu` shell script sets `LD_PRELOAD` for gtk4-layer-shell, which
must load before `libwayland-client`; under GJS the linker cannot arrange that
on its own. It is only needed for `--layer`.

## What is wired to the daemon, and what is not

The daemon exposes **no socket and no D-Bus interface** — it is a hold-to-talk
process that logs to stderr. Everything here binds to what actually exists:

| Panel | Source | Real? |
|---|---|---|
| status dot | `systemctl --user is-active whisprd` | yes |
| live transcript feed | `journalctl --user -u whisprd -f`, matching the `transcript:` prefix written by `src/main.c` | yes |
| microphone list | `pactl list sources` | yes |
| level meter | `parec` on the selected device, 48 bars at ~31fps | yes |
| all five settings panels | `~/.config/whisprd/config.ini` | yes |
| config reload | `SIGHUP` to the daemon | yes |
| recent sessions, file count, open, delete | `~/.local/share/whisprd/transcriptions` | yes |

### The level meter

`parec` on the selected device at 16 kHz mono — the same rate the daemon
captures at — RMS per ~32 ms chunk, so it updates at about 31 fps and reflects
what whisprd actually hears. Bars scroll right-to-left, so the meter reads as
recent history rather than one number smeared across 48 columns.

The silence threshold is learned rather than fixed. Noise floors vary far more
between devices than a constant can absorb: measured on this machine, a
desktop analog input idles at RMS 729 while a USB camera mic idles at 162. A
fixed threshold would show a quarter-full bar in a silent room on one and
swallow quiet speech on the other. So the quietest chunk in the last ~3 s is
taken as silence for whichever device is open, which puts both near zero at
rest. Speech is not continuous, so its gaps keep that minimum honest.

Astal is deliberately not in this path. `AstalCava` can only be aimed at a
source by assigning `cava.source`, which aborts the process with
`free(): double free detected in tcache 2` on the packaged snapshot
(`astal 0~9.git7f2292f`).

## History

`src/history.c` in the daemon appends each transcript to a session file.
Consecutive utterances share a file until five minutes of silence starts a new
one, and the file is named after the first thing said in it —
`20260720-091400-blockers-on-the-auth-rewrite.txt`. Files are `0600` in a
`0700` directory.

**Recording is off by default.** It is a verbatim record of everything spoken
at the machine, including anything dictated into a password field, so it is
opt-in:

```ini
history = on
```

The format is deliberately plain — one utterance per line, no markup — so
`cat`, `grep` and `$EDITOR` all work on it and this panel is not the only way
to reach your own transcripts.

### Deleting

Each row reveals a `✕` on hover, and `Delete` removes the selected row. Both
**move the file to the trash rather than unlinking it**, so there is no
confirmation step: a one-click control on a hover target is easy to hit by
accident, and these files are the only copy of something you said. Recover
from your desktop's trash as normal. Where there is no trash (a tmpfs, some
sandboxes) it falls back to a real delete.

### Keys

| | |
|---|---|
| `↑` `↓` / `k` `j` | move selection |
| `⏎` | open the selected transcript |
| `Delete` | move the selected transcript to the trash |
| `^E` | open the transcripts folder |
| `Esc` / `^X` | close |

## Config

Reads and writes `~/.config/whisprd/config.ini` in the daemon's own format:
flat `key = value` lines, no section headers. `GLib.KeyFile` is deliberately
not used — it requires a group header, and writing one would make the daemon
log a warning on every start.

The handoff spec asked for TOML (`[hotkey] binding`, `[endpoint] provider`).
That would not load: the daemon's parser (`src/config.c`) reads the INI keys
`hotkey`, `endpoint_url`, `model`, `api_key`, `source`, `backend`, `layout`,
`variant`, `paste_chord`. The panels map onto those.

### The API key

An editable entry, masked until `[show]`, never logged. The file is written
`0600`.

It is saved on `Enter` and on focus-out, not per keystroke: saving runs
`persist()`, which SIGHUPs the daemon and rebuilds its whole capture stack.
Doing that once per character would restart the daemon fifty times for one
pasted key.

If `OPENAI_API_KEY` is set in the environment the field greys out and reads
*set by OPENAI_API_KEY*, because the daemon prefers the environment over the
file — showing the file's value there would name a key that is not the one in
use.

### Model presets

The daemon speaks one protocol, OpenAI multipart to `endpoint_url`
(`src/transcribe.c`), so the mock's `deepgram / nova-2` and `assemblyai` would
fail outright. The endpoint is no longer a choice either; the dropdown selects
the OpenAI model: `whisper-1`, `gpt-4o-transcribe`, `gpt-4o-mini-transcribe`.

### Hotkey capture

`[ set ]` records the next combo and writes `hotkey`. The daemon owns the
actual grab; on Wayland a true global grab lives in the compositor, so this
only records the binding.

## Daemon change

`src/main.c` gained a `SIGHUP` handler. Every subsystem captures its settings
at init — the hotkey in the evdev loop, the endpoint in the curl handle, the
source in the capture stream — so applying a change means tearing the stack
down and standing it back up. The handler sets a flag and stops the input
loop; `main` then re-reads the config and re-enters setup. Same process, same
uinput device, fresh config. `SIGINT`/`SIGTERM` still exit.

## Deviations from the spec

- **The title bar was removed.** The mock's three dots imitated window
  controls, but this is a layer-shell surface with no chrome — they could not
  be clicked and did nothing. The config path it carried moved under the host
  header, where it is still true.
- **`^E` opens the folder** rather than exporting. The sessions are already
  plain `.txt` on disk, so an export step would only be a copy.
- **Session filenames are derived from speech**, not chosen by the user as the
  mock's `standup-notes.txt` implies — the daemon has no way to know what you
  would have called it.
- **Letter-spacing** on legends is a Pango attribute; GTK4 CSS has no
  `letter-spacing` property.

## Installing

`make install` from the top level installs the daemon and this panel together;
`make WITH_MENU=0` leaves the panel out. Either way the launcher is rewritten on
the way past to point at `$(PREFIX)/share/whisprd-menu`, since `app.js` no
longer sits beside it once it lands in `bin/`. `SCRIBE_MENU_DIR` overrides it.

## Replaces `whisprd-gui`

This took over from the GTK4 `whisprd-gui`, which has been removed along with
`src/gui/` and the `WITH_GUI` build flag. It was a second editor of the same
`config.ini` that rewrote the file from a fixed template with no `history` or
`history_dir` field, so saving in it silently turned off transcript recording
this panel had enabled.

Three of its settings have no panel here yet — `backend`, `paste_chord`, and the
`whisprd --say` injection test. The daemon still reads all three, and
`src/config.js` keeps them in `KEYS[]` so they survive a save untouched; they
just need hand-editing for now. See `TODO.md`.
