/* Read/write ~/.config/whisprd/config.ini.
 *
 * The daemon's parser (src/config.c) reads flat `key = value` lines with no
 * section headers, so GLib.KeyFile is unusable here -- it requires a group
 * header, and writing one would make the daemon log a warning for it on every
 * start. We parse and emit the daemon's own format instead, preserving the
 * comments from config.ini.example so a hand-edited file stays readable. */

import GLib from "gi://GLib";
import Gio from "gi://Gio";

export const KEYS = [
    "hotkey", "source", "endpoint_url", "model", "api_key",
    "backend", "layout", "variant", "paste_chord",
    "history", "history_dir",
];

/* The daemon accepts four spellings for on (src/config.c), so a file written by
 * hand with `history = true` is on as far as whisprd is concerned. Comparing
 * against the string "on" alone reports it as off. */
export function isOn(value) {
    return ["on", "true", "yes", "1"].includes(String(value ?? "").trim().toLowerCase());
}

export function path() {
    const xdg = GLib.getenv("XDG_CONFIG_HOME");
    const base = xdg && xdg.length ? xdg : GLib.build_filenamev([GLib.get_home_dir(), ".config"]);
    return GLib.build_filenamev([base, "whisprd", "config.ini"]);
}

/* Mirrors the daemon's defaults in config_load(), so an absent file shows the
 * same values the daemon would actually be running with. */
export function defaults() {
    return {
        hotkey: "KEY_RIGHTCTRL",
        source: "",
        endpoint_url: "https://api.openai.com/v1",
        model: "whisper-1",
        api_key: "",
        backend: "auto",
        layout: "us",
        variant: "",
        paste_chord: "ctrl+v",
        /* Matches the daemon's default: recording is opt-in. */
        history: "off",
        history_dir: "",
    };
}

/* Mirrors strip_comment() in src/config.c: a '#' only starts a comment at the
 * start of the line or after whitespace, so one inside a value survives.
 *
 * Cutting at the first '#' instead -- which this used to do -- reads a shorter
 * value than the daemon does, and save() then writes that truncation back over
 * the real one. The next unrelated change in the panel is enough to trigger it,
 * and for api_key the damage is silent and permanent. */
function stripComment(line) {
    for (let i = 0; i < line.length; i++) {
        if (line[i] !== "#") continue;
        if (i === 0 || /\s/.test(line[i - 1])) return line.slice(0, i);
    }
    return line;
}

export function load() {
    const cfg = defaults();
    const p = path();
    if (!GLib.file_test(p, GLib.FileTest.EXISTS))
        return cfg;

    const [ok, bytes] = GLib.file_get_contents(p);
    if (!ok) return cfg;

    const text = new TextDecoder().decode(bytes);
    for (let line of text.split("\n")) {
        line = stripComment(line);
        const eq = line.indexOf("=");
        if (eq < 0) continue;
        const key = line.slice(0, eq).trim();
        const val = line.slice(eq + 1).trim();
        if (KEYS.includes(key)) cfg[key] = val;
    }
    return cfg;
}

/* Written in the same shape and comment order as config.ini.example so the
 * file stays hand-editable after the GUI touches it. */
export function save(cfg) {
    const p = path();
    const dir = GLib.path_get_dirname(p);
    GLib.mkdir_with_parents(dir, 0o700);

    const out =
`# whisprd configuration -- written by scribe-menu

# --- hotkey ---
# evdev key name to hold. Single key or "MOD+KEY".
hotkey = ${cfg.hotkey}

# --- capture ---
# pulse/pipewire source name; empty = system default.
source = ${cfg.source}

# --- history ---
# on = keep every transcript on disk, for the menu's session list.
history = ${cfg.history}
# empty = ~/.local/share/whisprd/transcriptions
history_dir = ${cfg.history_dir}

# --- openai ---
# Required. Get one at https://platform.openai.com/api-keys
# $OPENAI_API_KEY in the environment overrides this.
api_key      = ${cfg.api_key}
model        = ${cfg.model}
endpoint_url = ${cfg.endpoint_url}

# --- injection ---
# backend = auto | wlr-vk | uinput | clipboard
backend = ${cfg.backend}
layout  = ${cfg.layout}
variant = ${cfg.variant}
paste_chord = ${cfg.paste_chord}
`;

    /* PRIVATE creates the file owner-only from the outset. Writing at the umask
     * default and chmodding afterwards leaves a window -- short, but on every
     * single save -- where an OpenAI API key is world-readable. The chmod stays
     * for the case the flag cannot honour: a file that already exists keeps the
     * permissions it already had. */
    const f = Gio.File.new_for_path(p);
    f.replace_contents(new TextEncoder().encode(out), null, false,
                       Gio.FileCreateFlags.REPLACE_DESTINATION |
                       Gio.FileCreateFlags.PRIVATE, null);

    GLib.chmod(p, 0o600);
    return p;
}
