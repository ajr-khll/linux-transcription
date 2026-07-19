# whisprd-menu

Floating settings + history panel for the whisprd daemon — design `4b`, built
on GJS + GTK4 + Astal.

```
cd menu
make deps      # check runtime dependencies
make run       # open the panel
```

`Esc` or `^X` closes it.

## Dependencies

Everything except the font is already installed on this machine:

| | |
|---|---|
| `gjs` | GTK4 JavaScript runtime |
| `astal-gtk4`, `astal-libs` | `AstalCava` (level meter) |
| `pactl` | capture device names |
| `gtk4-layer-shell` | only for `--layer` mode |
| **IBM Plex Mono** | **not installed** — `sudo dnf install ibm-plex-mono-fonts` |

Without the font the layout is correct but rendered in the wrong typeface.

> Fedora's `dnf install ags` is **Adventure Game Studio**, an unrelated project
> that happens to share the name. Aylur's shell is the `astal*` packages, from
> the `solopasha/hyprland` COPR.

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

### Overlay mode

`whisprd-menu --layer` (or `WHISPRD_MENU_LAYER=1`) makes it a layer-shell
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
| level meter | `AstalCava`, 48 bars, throttled to 30fps | **no — see below** |
| all five settings panels | `~/.config/whisprd/config.ini` | yes |
| config reload | `SIGHUP` to the daemon | yes |
| recent sessions, file count, open, delete | `~/.local/share/whisprd/transcriptions` | yes |

### Known issue: the level meter does not move

It does not follow the selected microphone. AstalCava defaults to source
`auto` — the default *output* monitor — so with nothing playing it reads zero.

Pointing it at the capture device is the obvious fix, but assigning
`cava.source` on the packaged snapshot (`astal 0~9.git7f2292f`) aborts the
process with `free(): double free detected in tcache 2`. The assignment is
deliberately not made: a meter is not worth crashing the panel for.

The fix is to drop Cava here and read the device directly —
`parec --device=<source> --format=s16le --rate=16000 --channels=1` into an RMS
calculation gives a true level for the selected microphone and removes the
dependency entirely. Not yet done.

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

The API key is masked by default, revealed only via `[show]`, and never
logged. The file is written `0600`.

### Endpoint presets

The mock offers `deepgram / nova-2` and `assemblyai`. The daemon speaks one
protocol — OpenAI-compatible multipart to `endpoint_url` (`src/transcribe.c`) —
so those would fail outright. The dropdown offers the presets that work: local
whisper.cpp on :8080 or :9000, and OpenAI.

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

## Relationship to `whisprd-gui`

This is intended to replace the GTK4 `whisprd-gui` (`src/gui/`), which covers
the same settings. Both are installed for now; once this reaches parity,
`whisprd-gui` and its Makefile target should be removed.
