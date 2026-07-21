/* Client side of the existing scribe daemon.
 *
 * The daemon exposes no socket and no D-Bus interface -- it is a hold-to-talk
 * process that logs to stderr. So "status" is the systemd user unit's state,
 * the live transcript feed is its journal, and "reload" is SIGHUP (see the
 * handler added to src/main.c). Nothing here invents a protocol. */

import GLib from "gi://GLib";
import Gio from "gi://Gio";

const UNIT = "scribe.service";

/* ---- status -------------------------------------------------------- */

export function isActive() {
    try {
        const [ok, out] = GLib.spawn_command_line_sync(
            `systemctl --user is-active ${UNIT}`);
        return ok && new TextDecoder().decode(out).trim() === "active";
    } catch (_) {
        return false;
    }
}

/* ---- config reload ------------------------------------------------- */

/* The daemon re-reads its config on SIGHUP. Falls back to a unit restart if
 * scribe is running outside systemd or the signal cannot be delivered. */
export function reload() {
    try {
        const [, , , status] = GLib.spawn_sync(
            null, ["systemctl", "--user", "kill", "-s", "SIGHUP", UNIT],
            null, GLib.SpawnFlags.SEARCH_PATH, null);
        if (status === 0) return "reloaded";
    } catch (_) { /* fall through */ }

    try {
        GLib.spawn_command_line_sync(`pkill -HUP -x scribe`);
        return "reloaded";
    } catch (_) {
        return "failed";
    }
}

/* ---- live transcript feed ------------------------------------------ */

/* Follows the journal for the user unit and emits the text of each
 * "scribe: transcript: ..." line. The match below keys on "transcript:"
 * alone, not the "scribe: " prefix, so renaming the daemon does not break
 * the feed -- but the worker thread in src/main.c owns the "transcript:"
 * marker, and if that changes it has to change here too. */
export class Feed {
    constructor(onLine) {
        this._onLine = onLine;
        this._proc = null;
        this._cancel = new Gio.Cancellable();
    }

    start() {
        if (this._proc) return;
        try {
            this._proc = Gio.Subprocess.new(
                ["journalctl", "--user", "-u", UNIT, "-f", "-n", "40",
                 "-o", "cat", "--no-pager"],
                Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE);
        } catch (e) {
            logError(e, "scribe-menu: cannot follow the journal");
            return;
        }
        const stream = new Gio.DataInputStream({
            base_stream: this._proc.get_stdout_pipe(),
            close_base_stream: true,
        });
        this._read(stream);
    }

    _read(stream) {
        stream.read_line_async(GLib.PRIORITY_DEFAULT, this._cancel, (s, res) => {
            let line = null;
            try {
                [line] = s.read_line_finish_utf8(res);
            } catch (_) {
                return;                       /* cancelled or stream closed */
            }
            if (line === null) return;        /* EOF */

            const m = line.match(/transcript:\s*(.*)$/);
            if (m && m[1].trim().length)
                this._onLine(m[1].trim());

            this._read(s);
        });
    }

    stop() {
        this._cancel.cancel();
        if (this._proc) {
            try { this._proc.force_exit(); } catch (_) { /* already gone */ }
            this._proc = null;
        }
    }
}
