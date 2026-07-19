/* Capture devices and the live input level.
 *
 * Device list comes from WirePlumber via AstalWp. The level meter comes from
 * AstalCava, which publishes normalised bar values at ~60fps -- we throttle to
 * the ~15-30fps the spec asks for. AstalWp exposes volume (the setting), not a
 * peak, which is why the meter needs Cava rather than Wp. */

import GLib from "gi://GLib";

let AstalWp = null, AstalCava = null;
try { AstalWp = (await import("gi://AstalWp")).default; } catch (_) { /* optional */ }
try { AstalCava = (await import("gi://AstalCava")).default; } catch (_) { /* optional */ }

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

/* KNOWN BROKEN: the meter does not follow the selected microphone.
 *
 * AstalCava defaults to source "auto", which is the default *output* monitor,
 * so with nothing playing it sits at zero -- the flat meter you see. The fix
 * would be to assign the capture node name to `cava.source`, but doing that on
 * the packaged snapshot (astal 0~9.git7f2292f) aborts the process with
 * "free(): double free detected in tcache 2". A meter is not worth crashing
 * the panel for, so the assignment is deliberately not made.
 *
 * The way out is to stop using Cava for this and read the device directly:
 * `parec --device=<source> --format=s16le --rate=16000 --channels=1` piped
 * into an RMS calculation gives a true level for the selected microphone with
 * no dependency on Cava at all. */
export class Meter {
    /* bars: the spec's 48. onValues: (Float[]) => void, normalised 0..1. */
    constructor(bars, onValues) {
        this.bars = bars;
        this._onValues = onValues;
        this._cava = null;
        this._sig = 0;
        this._last = 0;
        this._minIntervalMs = 1000 / 30;   /* spec: ~15-30fps */
    }

    start() {
        if (!AstalCava) return false;
        this._cava = AstalCava.get_default();
        if (!this._cava) return false;

        this._cava.bars = this.bars;
        this._cava.active = true;

        this._sig = this._cava.connect("notify::values", () => {
            const now = GLib.get_monotonic_time() / 1000;
            if (now - this._last < this._minIntervalMs) return;
            this._last = now;
            this._onValues(this._cava.values);
        });
        return true;
    }

    stop() {
        if (this._cava && this._sig) {
            this._cava.disconnect(this._sig);
            this._sig = 0;
        }
        this._cava = null;
    }
}

export const hasMeter = () => AstalCava !== null;
