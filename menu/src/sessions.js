/* Reads the session files the daemon writes (src/history.c).
 *
 * Format is deliberately plain: one utterance per line, no markup, named
 * <YYYYMMDD-HHMMSS>-<slug>.txt. That keeps the files useful on their own --
 * cat, grep and $EDITOR all work -- so nothing here needs to be the only way
 * to get at them. */

import GLib from "gi://GLib";
import Gio from "gi://Gio";

export function dir() {
    const xdg = GLib.getenv("XDG_DATA_HOME");
    const base = xdg && xdg.length
        ? xdg
        : GLib.build_filenamev([GLib.get_home_dir(), ".local", "share"]);
    return GLib.build_filenamev([base, "whisprd", "transcriptions"]);
}

/* 612w / 3.2kw, matching the design. */
function formatWords(n) {
    if (n < 1000) return `${n}w`;
    return `${(n / 1000).toFixed(1)}kw`;
}

/* Recent entries want a clock, older ones want a day. A list where every row
 * reads "20260720-015909" is not scannable.
 *
 * Comparison is against midnight rather than a rolling 24 hours, so something
 * dictated at 23:50 reads "yest" the next morning instead of "12:00". */
function formatWhen(dt) {
    const now = GLib.DateTime.new_now_local();
    const midnight = GLib.DateTime.new_local(
        now.get_year(), now.get_month(), now.get_day_of_month(), 0, 0, 0);

    if (dt.to_unix() >= midnight.to_unix())
        return dt.format("%H:%M");

    /* Whole days between dt and the start of today: 0 == yesterday. */
    const days = Math.floor((midnight.to_unix() - dt.to_unix()) / 86400);
    if (days < 1) return "yest";
    if (days < 6) return dt.format("%a").toLowerCase();
    return dt.format("%d %b").toLowerCase();
}

/* The filename records when the session started, which is what the list is
 * labelling. mtime is the last append, and it also changes if the file is
 * copied about, so it is only the fallback. */
function timestampOf(info, name) {
    const m = name.match(/^(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})/);
    if (m)
        return GLib.DateTime.new_local(+m[1], +m[2], +m[3], +m[4], +m[5], +m[6]);

    return info.get_modification_date_time() ?? GLib.DateTime.new_now_local();
}

/* Returns newest-first. Each entry: { path, name, when, snippet, words }. */
export function list() {
    const d = dir();
    if (!GLib.file_test(d, GLib.FileTest.IS_DIR))
        return [];

    const out = [];
    let en;
    try {
        en = Gio.File.new_for_path(d).enumerate_children(
            "standard::name,standard::size,time::modified",
            Gio.FileQueryInfoFlags.NONE, null);
    } catch (e) {
        logError(e, "whisprd-menu: cannot read the history directory");
        return [];
    }

    let info;
    while ((info = en.next_file(null)) !== null) {
        const name = info.get_name();
        if (!name.endsWith(".txt")) continue;

        const path = GLib.build_filenamev([d, name]);
        let text = "";
        try {
            const [ok, bytes] = GLib.file_get_contents(path);
            if (ok) text = new TextDecoder().decode(bytes);
        } catch (_) {
            continue;                       /* unreadable; skip rather than crash */
        }

        const lines = text.split("\n").filter(l => l.trim().length);
        const words = text.split(/\s+/).filter(w => w.length).length;
        const dt = timestampOf(info, name);

        out.push({
            path,
            /* The timestamp prefix is redundant next to the time column. */
            name: name.replace(/^\d{8}-\d{6}-/, ""),
            when: formatWhen(dt),
            sortKey: dt.to_unix(),
            snippet: lines.length ? lines[0] : "(empty)",
            words: formatWords(words),
        });
    }

    out.sort((a, b) => b.sortKey - a.sortKey);
    return out;
}

export function count() {
    return list().length;
}

export function read(path) {
    try {
        const [ok, bytes] = GLib.file_get_contents(path);
        return ok ? new TextDecoder().decode(bytes) : "";
    } catch (_) {
        return "";
    }
}

/* Opens in whatever handles text/plain. */
export function open(path) {
    try {
        Gio.AppInfo.launch_default_for_uri(
            Gio.File.new_for_path(path).get_uri(), null);
        return true;
    } catch (e) {
        logError(e, "whisprd-menu: cannot open the transcript");
        return false;
    }
}

/* Moves to the trash rather than unlinking. A one-click control on a hover
 * target is easy to hit by accident, and these are the only copy of something
 * the user said -- recoverable is the right default, and it means the button
 * needs no confirmation step. Falls back to delete where there is no trash
 * (a tmpfs, or some sandboxes). */
export function remove(path) {
    const f = Gio.File.new_for_path(path);
    try {
        f.trash(null);
        return "trashed";
    } catch (_) {
        try {
            f.delete(null);
            return "deleted";
        } catch (e) {
            logError(e, "whisprd-menu: cannot remove the transcript");
            return "failed";
        }
    }
}
