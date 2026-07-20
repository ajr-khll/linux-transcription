# Paste this into Claude Code

Copy the block below into Claude Code, run from your existing whisprd repo. Read `README.md`
in this same folder first — it has the full pixel spec for the menu UI (design "4b").

---

I already have the **whisprd daemon** (mic capture, transcription, history). I now need ONLY
the **menu GUI** — a lightweight, native, floating-center-screen TUI. **No Electron, no web
stack, no daemon changes.**

Read `README.md` in this folder for the exact layout/colors/copy, and open
`Whisprd Menu.dc.html` → option `4b` for the look.

Build **`whisprd-menu`**: a single small binary (a real terminal UI) that draws design `4b`,
reads/writes `~/.config/whisprd/config.toml`, and acts as a **client of my existing daemon**.

Requirements:
- **Rust + `ratatui`** (single static binary, minimal deps: `serde`/`toml`, `ureq` only if
  needed). Go+bubbletea is an acceptable alternative if you think it's lighter for this.
- Render the two-column `4b` layout exactly: `#0c0c0c` bg, IBM Plex Mono, red `#ff4b4b` accent,
  the five fieldset settings panels on the left, and on the right a live `tail -f` transcript
  feed (blinking block cursor on the newest line) + a `recent sessions` list + a function-key
  footer (`⏎ open  / search  ^E export  ^X close`).
- **Talk to my existing daemon** — first inspect its interface in this repo (Unix socket /
  CLI / D-Bus) and bind to the REAL protocol. The menu needs: read status, stream live
  transcript lines, list recent sessions, open a session, and trigger a config reload after a
  change. Ask me if the daemon's interface isn't obvious from the source.
- Config: read/write the TOML keys shown in the mock (`[hotkey] binding`, `[microphone] device`,
  `[endpoint] provider`/`api_key`, `layout`). Never log the API key. The hotkey `[ set ]`
  captures a combo and writes `binding` — the daemon owns the actual grab.
- Mic meter animates from the live level (from the daemon if it publishes one; otherwise read
  the device directly just for the meter).
- It must **float center-screen, not fullscreen** — no window chrome. Provide example
  Sway/Hyprland + i3 window rules to float `whisprd-menu` at 940×566 centered, plus a keybind
  that spawns it in a floating terminal (`foot --app-id=whisprd-menu -e whisprd-menu`).
- Include a `justfile`/`Makefile` and a short README with build/run/keybind steps.

Start by inspecting my daemon's interface and scaffolding the `whisprd-menu` binary + config
types, show me the plan, then implement the `4b` layout before wiring it to the daemon.
