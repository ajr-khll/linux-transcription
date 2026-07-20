# Handoff: whisprd — floating TUI menu (design 4b)

## Overview
`whisprd` is a live-narration tool for Linux. **The daemon already exists** (mic capture,
transcription, history). This handoff covers ONLY the **menu GUI** — design `4b`, a
terminal-style panel that floats center-screen. It shows all settings (hotkey, microphone,
transcription endpoint, API key, keyboard layout) and a **transcription history** view, and
talks to the existing daemon.

## About the design file
`Whisprd Menu.dc.html` in this bundle is a **design reference built in HTML** — it shows the
intended look and behavior of design `4b`. It is not the app. Reimplement `4b` as a
lightweight native TUI per the spec below. Other options (1a–4a) in the file are alternative
directions; ignore them.

## Fidelity
**High-fidelity.** Colors, type, spacing and copy in `4b` are final. Match them exactly.

---

## Build target — as lightweight as possible

Design `4b` is a terminal UI, so build a **real TUI**, not a GUI toolkit, and definitely not
Electron. One small binary:

```
whisprd-menu   # the 4b TUI: reads/writes config, talks to the existing daemon, exits
```

### Language / TUI library (pick one)
- **Rust + `ratatui`** (recommended) — single static binary, ~1–3 MB, no runtime deps.
- **Go + `bubbletea`/`lipgloss`** — also a single static binary, ergonomic for this layout.
- **C + `notcurses`/`ncurses`** — smallest footprint if that's the priority.

Avoid GTK/Qt: replicating the terminal look in a widget toolkit is more code and more weight
than rendering it in a terminal.

### "Hovering center screen, not fullscreen"
Don't draw window chrome. Launch `whisprd-menu` inside a small floating terminal and let the
window manager center/float it:
- **Sway/Hyprland**: `for_window [app_id="whisprd-menu"] floating enable, resize set 940 566, move position center`
  then bind a key to `foot --app-id=whisprd-menu -e whisprd-menu` (or `kitty`/`alacritty`).
- **X11 (i3/bspwm)**: equivalent floating rule + a keybind spawning the terminal.

That WM rule is what makes it feel like the floating panel in the mock while staying native.

### Talking to the existing daemon
The menu is a **client**. Wire it to whatever interface the daemon already exposes — adapt
these to the real protocol:
- If the daemon has a **Unix socket** (e.g. `$XDG_RUNTIME_DIR/whisprd.sock`): connect and send
  its commands for status, live transcript stream, and history listing.
- If it has a **CLI/D-Bus**: shell out / call those instead.
- The menu needs, at minimum: read **status** (active?), stream **live transcript lines** (the
  `tail -f` feed), request **recent sessions / history**, open a session, and tell the daemon
  to **reload** config after a change.

> Point Claude Code at the daemon's actual source/socket protocol so it binds to the real API
> rather than inventing one.

### Config (shared with the daemon)
- Read/write **`~/.config/whisprd/config.toml`** — keys as shown in the mock (`[hotkey] binding`,
  `[microphone] device`, `[endpoint] provider` + `api_key`, `layout`). After a write, tell the
  daemon to reload. Never log the API key.

### Mic level meter
- Draw from the daemon's live RMS/level value if it publishes one; otherwise the meter can read
  the same input device directly at ~15–30 fps just for visualization.

### Global hotkey (display + capture only)
- The menu's hotkey panel just **captures a combo and writes `[hotkey] binding`** to the config;
  the daemon owns the actual grab. (Note: on Wayland global grabs live in the compositor —
  document binding `whisprd toggle` in Sway/Hyprland if the daemon relies on that.)

---

## Screen spec — design 4b

Overall: **940×566** terminal window, `#0c0c0c` bg, 1px `#2a2a2a` border, radius 8, font
**IBM Plex Mono** throughout. Title bar + two columns.

**Title bar** (`#151515`, 1px bottom `#2a2a2a`): three dots (`#ff4b4b`, `#3a3a3a`, `#3a3a3a`)
left; centered dim label `whisprd — dashboard — ~/.config/whisprd` (`#8a8a8a`, 11px).

**Left column — settings** (width 392, right border `#2a2a2a`, 13px pad, body text `#c8c8c8` 12px):
- Header line: `whisprd@linux · ● active · pipewire` (`@linux` `#5a5a5a`, dot `#ff4b4b`).
- Five `fieldset` panels, 1px `#333`, radius 4, 11px gap. Legends red (`#ff6a6a`, 10px,
  letter-spacing .08em): `─ hotkey ─`, `─ microphone ─`, `─ endpoint ─`, `─ api_key ─`,
  `─ kb_layout ─`.
  - **hotkey**: key chips (transparent, 1px `#ff4b4b`, `#ff6a6a`) + `[ set ]` button
    (transparent, 1px `#ff4b4b`); while recording it reads `AWAITING INPUT…` and the button
    reads `[esc]` on a solid `#ff4b4b` fill (`#0c0c0c` text).
  - **microphone**: `<select>` `hw:0,0 — Built-in Audio` / `bluez — AirPods Pro` /
    `hw:2,0 — Shure MV7`, then a live level meter — thin red bars (`#ff4b4b`, ~48 bars).
  - **endpoint**: `<select>` `openai / whisper-1` · `deepgram / nova-2` · `assemblyai` ·
    `localhost:9000 — whisper.cpp`.
  - **api_key**: masked value + `[show]`/`[hide]` toggle (`#ff6a6a`).
  - **kb_layout**: `<select>` `us — qwerty` · `us — dvorak` · `us — colemak` · `fr — azerty`.
  - Selects: `#0c0c0c` bg, 1px `#333`, radius 2, `#d8d8d8` text, custom `▾`.

**Right column — history / log** (flex, `#c8c8c8` 12px):
- Sub-header (1px bottom `#1e1e1e`): `transcriptions/` (`#ff6a6a`) · dim `grep ▏` · right `128 files`.
- Live feed: `$ tail -f session.log` (`#5a5a5a`), then transcript lines each prefixed with a
  red timestamp `HH:MM:SS` (`#ff6a6a`); the newest line ends in a blinking block cursor
  (7×13, `#ff4b4b`, 1.1s step blink).
- Divider: `──────── recent sessions ────────` (`#3a3a3a`).
- Session rows: red time col (`#ff6a6a`, 92px) · filename + snippet (`#d8d8d8`, ellipsized) ·
  word count (`#7a7a7a`). Example rows: `standup-notes.txt … 612w`, `novel-ch7.md … 3.2kw`,
  `memo.txt … 96w`, `research-call.txt … 6.9kw`, `dev-log.md … 1.1kw`.
- Footer bar (`#151515`, 1px top `#2a2a2a`, 10.5px `#8a8a8a`): `⏎ open` · `/ search` ·
  `^E export` · (right) `^X close`. Function keys in `#ff4b4b` bold.

### Palette
`#0c0c0c` bg · `#151515` bars · `#2a2a2a`/`#333`/`#3a3a3a` borders · `#c8c8c8`/`#d8d8d8` text ·
`#8a8a8a`/`#7a7a7a`/`#5a5a5a` dim · accent red `#ff4b4b`, lighter `#ff6a6a`/`#ff9a9a`.

### Interactions
- Hotkey `[ set ]` → capture mode, grab next combo, write `[hotkey] binding` to TOML, tell
  daemon to reload.
- Mic meter animates from the live level.
- `[show]/[hide]` reveals/masks the API key.
- Selects persist to `config.toml` immediately + trigger daemon reload.
- History rows: `⏎`/click opens the full transcript; `/` filters; `^E` exports.

## Files
- `Whisprd Menu.dc.html` — HTML design reference (design `4b`; also contains 1a–4a).
