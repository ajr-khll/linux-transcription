/* Capture devices and the live input level.
 *
 * Both read the device the daemon is actually configured to use, via pactl
 * and parec. Astal is deliberately not in this path: AstalWp reports a null
 * `name` for capture endpoints, and AstalCava can only be pointed at a source
 * by assigning `cava.source`, which aborts the process on the packaged
 * snapshot. parec gives a true level for the selected microphone and cannot
 * crash on a property assignment. */

import GLib from "gi://GLib";
import Gio from "gi://Gio";

let AstalWp = null;
try { AstalWp = (await import("gi://AstalWp")).default; } catch (_) { /* optional */ }

/* ---- capture devices ----------------------------------------------- */

/* Deliberately not AstalWp. Its endpoints expose `description` and a `path`
 * like "alsa:acp:Camera:0:capture", but `name` is null -- and the daemon's
 * `source =` key matches on the PipeWire node name ("alsa_input.pci-...").
 * Writing anything else silently selects nothing.
 *
 * pactl reports the names the daemon actually matches, and does so
 * synchronously, which also avoids AstalWp's other trap: WirePlumber has not
 * enumerated anything until the main loop has ticked, so a list built during
 * window construction comes back empty. */
function pactlSources() {
    let out;
    try {
        const [ok, stdout, , status] =
            GLib.spawn_command_line_sync("pactl list sources");
        if (!ok || status !== 0) return null;
        out = new TextDecoder().decode(stdout);
    } catch (_) {
        return null;
    }

    const found = [];
    let cur = null;
    for (const raw of out.split("\n")) {
        const line = raw.trim();
        if (/^Source #\d+/.test(line)) {
            if (cur && cur.name) found.push(cur);
            cur = { name: "", description: "", monitor: false };
            continue;
        }
        if (!cur) continue;

        let m;
        if ((m = line.match(/^Name:\s*(.+)$/))) cur.name = m[1];
        else if ((m = line.match(/^Description:\s*(.+)$/))) cur.description = m[1];
        else if ((m = line.match(/^Monitor of Sink:\s*(.+)$/)))
            cur.monitor = m[1] !== "n/a";
    }
    if (cur && cur.name) found.push(cur);

    /* Monitors are loopbacks of output, not microphones. */
    return found.filter(s => !s.monitor);
}

/* Fallback when pactl is missing: labels only. The daemon cannot be pointed at
 * these, so they are shown but not selectable as a source name. */
function astalDescriptions() {
    if (!AstalWp) return [];
    const audio = AstalWp.get_default()?.get_audio();
    if (!audio) return [];
    return audio.get_microphones().map(m => ({
        name: "",
        description: m.description ?? `node ${m.id}`,
        monitor: false,
    }));
}

/* Returns [{ name, description }]; an empty name means "system default". */
export function sources() {
    const list = [{ name: "", description: "system default" }];
    for (const s of pactlSources() ?? astalDescriptions())
        list.push({ name: s.name, description: s.description || s.name });
    return list;
}

/* ---- level meter ---------------------------------------------------- */

/* Same rate the daemon captures at, so the meter reflects what whisprd hears
 * rather than a resampled approximation of it. */
const RATE = 16000;
/* 512 samples, mono s16 -- one chunk every ~32ms, so the meter updates at
 * ~31fps without needing a throttle on top. */
const CHUNK = 1024;

/* Speech sits far below full scale, so a linear bar would barely leave the
 * floor. The square root opens up the quiet end where the useful signal is;
 * REF is roughly a comfortable speaking level mapping to a full bar. */
const REF = 5000;

/* Noise floors differ enormously between devices -- measured here, a desktop
 * analog input idles around RMS 729 while a USB camera mic idles at 162. A
 * fixed threshold would either show a quarter-full bar in a silent room or
 * swallow quiet speech, depending on which one is selected. So the floor is
 * learned instead: the quietest chunk in the last few seconds is taken as
 * silence for whatever device is currently open.
 *
 * Speech is not continuous, so the gaps keep the minimum honest even while
 * someone is talking. */
const FLOOR_WINDOW = 96;        /* ~3s at 32ms per chunk */
const FLOOR_MARGIN = 1.6;       /* clear of the floor before a bar shows */

export class Meter {
    /* bars: the spec's 48. onValues: (Float[]) => void, normalised 0..1. */
    constructor(bars, onValues) {
        this.bars = bars;
        this._onValues = onValues;
        /* Bars scroll right-to-left, so the meter reads as recent history
         * rather than a single number smeared across 48 columns. */
        this._ring = new Array(bars).fill(0);
        this._floor = [];
        this._proc = null;
        this._cancel = null;
        this._source = null;
        this._warned = false;
    }

    /* source: the PipeWire node name, or "" for the system default. */
    start(source) {
        this.stop();
        this._source = source ?? "";
        this._cancel = new Gio.Cancellable();
        /* The learned floor belongs to the previous device. */
        this._floor = [];

        const argv = ["parec", "--format=s16le", `--rate=${RATE}`, "--channels=1",
                      "--latency-msec=30"];
        if (this._source) argv.push(`--device=${this._source}`);

        try {
            this._proc = Gio.Subprocess.new(
                argv,
                Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE);
        } catch (e) {
            /* Once, not every time the device changes. */
            if (!this._warned) {
                logError(e, "scribe-menu: cannot run parec; the meter will stay flat");
                this._warned = true;
            }
            this._proc = null;
            return false;
        }

        this._read(this._proc.get_stdout_pipe());
        return true;
    }

    /* Restarts the capture against a different device. */
    setSource(source) {
        if ((source ?? "") === this._source) return;
        this.start(source);
    }

    _read(stream) {
        stream.read_bytes_async(CHUNK, GLib.PRIORITY_DEFAULT, this._cancel, (s, res) => {
            let bytes;
            try {
                bytes = s.read_bytes_finish(res);
            } catch (_) {
                return;                         /* cancelled, or parec exited */
            }
            if (!bytes || bytes.get_size() === 0)
                return;                         /* EOF */

            this._push(bytes.get_data());
            this._read(s);
        });
    }

    _push(u8) {
        const n = Math.floor(u8.length / 2);
        if (!n) return;

        /* A DataView reads the little-endian samples without requiring the
         * buffer to be 2-byte aligned, which a Uint8Array slice need not be. */
        const view = new DataView(u8.buffer, u8.byteOffset, n * 2);
        let sum = 0;
        for (let i = 0; i < n; i++) {
            const v = view.getInt16(i * 2, true);
            sum += v * v;
        }

        const rms = Math.sqrt(sum / n);

        this._floor.push(rms);
        if (this._floor.length > FLOOR_WINDOW) this._floor.shift();
        const floor = Math.min(...this._floor) * FLOOR_MARGIN;

        const level = Math.min(1, Math.sqrt(Math.max(0, rms - floor) / REF));
        this._ring.push(level);
        this._ring.shift();
        this._onValues(this._ring);
    }

    stop() {
        if (this._cancel) {
            this._cancel.cancel();
            this._cancel = null;
        }
        if (this._proc) {
            try { this._proc.force_exit(); } catch (_) { /* already gone */ }
            this._proc = null;
        }
        this._source = null;
    }
}
