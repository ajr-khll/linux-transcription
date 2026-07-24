/* Read/write ~/.config/scribe/config.ini.
 *
 * The daemon's parser (src/config.c) reads flat `key = value` lines with no
 * section headers, so GLib.KeyFile is unusable here -- it requires a group
 * header, and writing one would make the daemon log a warning for it on every
 * start. We parse and emit the daemon's own format instead, preserving the
 * comments from config.ini.example so a hand-edited file stays readable. */

import GLib from "gi://GLib";
import Gio from "gi://Gio";

/* Every key the daemon reads has to be listed here, whether or not a panel
 * edits it. save() rewrites the whole file from this object, so anything
 * missing is dropped from the user's config on the next save -- silently, and
 * for a key like `engine` that means their local transcriber quietly becomes
 * an upload. */
export const KEYS = [
    "hotkey", "source", "engine", "model_dir", "threads",
    "live", "live_model_dir",
    "cleanup", "cleanup_endpoint_url", "cleanup_model", "cleanup_timeout_ms",
    "vocabulary_file",
    "endpoint_url", "model", "api_key",
    "backend", "layout", "variant", "paste_chord",
    "history", "history_dir",
];

/* The daemon accepts four spellings for on (src/config.c), so a file written by
 * hand with `history = true` is on as far as scribe is concerned. Comparing
 * against the string "on" alone reports it as off. */
export function isOn(value) {
    return ["on", "true", "yes", "1"].includes(String(value ?? "").trim().toLowerCase());
}

export function path() {
    const xdg = GLib.getenv("XDG_CONFIG_HOME");
    const base = xdg && xdg.length ? xdg : GLib.build_filenamev([GLib.get_home_dir(), ".config"]);
    return GLib.build_filenamev([base, "scribe", "config.ini"]);
}

/* Mirrors the daemon's defaults in config_load(), so an absent file shows the
 * same values the daemon would actually be running with. */
export function defaults() {
    return {
        hotkey: "KEY_RIGHTCTRL",
        source: "",
        engine: "openai",
        model_dir: "",
        threads: "0",
        /* Matches the daemon's default: opt-in, because the preview types a
         * guess into whatever window has focus. */
        live: "off",
        live_model_dir: "",
        /* Matches the daemon's default: off, and local when on. */
        cleanup: "off",
        cleanup_endpoint_url: "http://localhost:11434/v1",
        cleanup_model: "",
        cleanup_timeout_ms: "2500",
        vocabulary_file: "",
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
`# scribe configuration -- written by scribe-menu

# --- hotkey ---
# evdev key name to hold. Single key or "MOD+KEY".
hotkey = ${cfg.hotkey}

# --- capture ---
# pulse/pipewire source name; empty = system default.
source = ${cfg.source}

# --- history ---
# on = keep every transcript on disk, for the menu's session list.
history = ${cfg.history}
# empty = ~/.local/share/scribe/transcriptions
history_dir = ${cfg.history_dir}

# --- engine ---
# openai   = upload each utterance to OpenAI; needs api_key below.
# parakeet = transcribe on this machine; no key, nothing uploaded.
engine = ${cfg.engine}

# --- parakeet ---
# empty = /usr/local/share/scribe/models/parakeet-tdt-0.6b-v3-int8
model_dir = ${cfg.model_dir}
# decode threads; 0 = 4
threads = ${cfg.threads}

# --- live preview ---
# on = type words as you speak, then replace them with the transcript when you
# let go. Off by default. Needs engine = parakeet, a backend that can erase
# (not clipboard), and a hotkey that is not a modifier.
live = ${cfg.live}
# empty = /usr/local/share/scribe/models/streaming-zipformer-en
live_model_dir = ${cfg.live_model_dir}

# --- cleanup ---
# on = fix each transcript with a local instruction model: drop "um"/"uh",
# resolve spoken self-corrections ("Monday, uh, no Tuesday" -> "Tuesday"), add
# punctuation. Off by default. The endpoint must be on this machine -- the
# daemon refuses a remote one, so nothing spoken leaves the computer.
cleanup = ${cfg.cleanup}
cleanup_endpoint_url = ${cfg.cleanup_endpoint_url}
cleanup_model = ${cfg.cleanup_model}
# how long to wait for the model before keeping the raw transcript, in ms
cleanup_timeout_ms = ${cfg.cleanup_timeout_ms}
# terms to spell correctly, one per line; empty = ~/.config/scribe/vocabulary.txt
vocabulary_file = ${cfg.vocabulary_file}

# --- openai ---
# Required when engine = openai. Get one at https://platform.openai.com/api-keys
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
