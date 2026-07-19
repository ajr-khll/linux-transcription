#!/usr/bin/env -S gjs -m
/* whisprd-menu -- design 4b.
 *
 * A floating, centred settings + history panel for the whisprd daemon.
 * Run:  gjs -m app.js       (from this directory)
 *
 * Layer-shell places and sizes the window itself, so unlike a terminal build
 * this needs no compositor window rules. On X11 it falls back to a plain
 * centred Gtk.Window.
 *
 * Note: every widget is built inside activate(). Constructing GTK widgets at
 * module scope runs them before Gtk.Application has initialised GTK, which
 * segfaults. */

import Gtk from "gi://Gtk?version=4.0";
import Gdk from "gi://Gdk?version=4.0";
import GLib from "gi://GLib";
import Gio from "gi://Gio";
import Pango from "gi://Pango";
import System from "system";

import * as Config from "./src/config.js";
import * as Daemon from "./src/daemon.js";
import * as Audio from "./src/audio.js";
import * as Sessions from "./src/sessions.js";

const W = 940, H = 566, LEFT_W = 392, BARS = 48;
const HERE = GLib.path_get_dirname(import.meta.url.replace("file://", ""));

/* Opt-in, not the default. A layer surface is an overlay plane -- the
 * mechanism bars and notification popups use -- so the compositor gives it no
 * move, no ordinary focus, no alt-tab and no window rules. That is right for a
 * bar and wrong for a panel you interact with, so the default is a normal
 * toplevel and the compositor places it (see README for the float rules).
 * `--layer` restores the overlay behaviour for anyone who wants it. */
const USE_LAYER = System.programArgs.includes("--layer") ||
                  GLib.getenv("WHISPRD_MENU_LAYER") === "1";

let LayerShell = null;
if (USE_LAYER) {
    try { LayerShell = (await import("gi://Gtk4LayerShell")).default; }
    catch (_) { /* X11, or the library is absent */ }
}

/* ---- construction helpers ------------------------------------------- */

const label = (text, cls, opts = {}) => new Gtk.Label({
    label: text, xalign: 0, ...opts,
    css_classes: Array.isArray(cls) ? cls : (cls ? [cls] : []),
});

const box = (orientation, spacing, cls, children = []) => {
    const b = new Gtk.Box({
        orientation: orientation === "h" ? Gtk.Orientation.HORIZONTAL
                                         : Gtk.Orientation.VERTICAL,
        spacing,
        css_classes: Array.isArray(cls) ? cls : (cls ? [cls] : []),
    });
    for (const c of children) b.append(c);
    return b;
};

const spacer = (opts = {}) => new Gtk.Box(opts);

/* GTK4 CSS has no letter-spacing, so the spec's .08em on legends is applied
 * as a Pango attribute instead. */
function tracked(text, em) {
    const l = label(text, "legend");
    const attrs = Pango.AttrList.new();
    attrs.insert(Pango.attr_letter_spacing_new(Math.round(em * 10 * Pango.SCALE)));
    l.set_attributes(attrs);
    return l;
}

function field(legend, child) {
    const f = new Gtk.Frame({ css_classes: ["field"] });
    f.set_label_widget(tracked(legend, 0.08));
    f.set_child(child);
    return f;
}

function select(items, active, onChange) {
    const dd = Gtk.DropDown.new_from_strings(items);
    dd.add_css_class("sel");
    dd.set_selected(active < 0 ? 0 : active);
    dd.connect("notify::selected", () => onChange(dd.get_selected()));
    return dd;
}

/* OpenAI transcription models. The endpoint itself is no longer a choice --
 * endpoint_url still exists in config.ini for anyone pointing this at an
 * OpenAI-compatible server, but it is not a supported path, so the panel
 * selects the model and leaves the URL alone. */
const MODELS = [
    { text: "whisper-1",             model: "whisper-1" },
    { text: "gpt-4o-transcribe",     model: "gpt-4o-transcribe" },
    { text: "gpt-4o-mini-transcribe", model: "gpt-4o-mini-transcribe" },
];

const LAYOUTS = [
    { text: "us — qwerty",  layout: "us", variant: "" },
    { text: "us — dvorak",  layout: "us", variant: "dvorak" },
    { text: "us — colemak", layout: "us", variant: "colemak" },
    { text: "fr — azerty",  layout: "fr", variant: "" },
];

const MODS = [
    [Gdk.ModifierType.CONTROL_MASK, "KEY_LEFTCTRL"],
    [Gdk.ModifierType.ALT_MASK,     "KEY_LEFTALT"],
    [Gdk.ModifierType.SHIFT_MASK,   "KEY_LEFTSHIFT"],
    [Gdk.ModifierType.SUPER_MASK,   "KEY_LEFTMETA"],
];

/* ---- state ---------------------------------------------------------- */

const cfg = Config.load();
/* The daemon prefers OPENAI_API_KEY over the file, so when it is set the panel
 * must not present the file's value as the one in use -- it isn't. */
const envKey = GLib.getenv("OPENAI_API_KEY") ?? "";
let srcList = [];
let recording = false;
let keyShown = false;
let meterValues = new Array(BARS).fill(0);

let win, feed, levelMeter;
let statusDot, statusTxt, chipRow, setBtn, meter;
let keyEntry, keyToggle, cursor, feedBox;
let sessBox, countLabel, emptyNote;
let sessRows = [];          /* [{ entry, widget }] in display order */
let selected = 0;
let sessionRefresh = 0;     /* debounce timer for list rebuilds */
const feedRows = [];
const MAX_FEED = 8;

function persist() {
    Config.save(cfg);
    Daemon.reload();
}

/* ---- hotkey panel ---------------------------------------------------- */

function renderChips() {
    let c = chipRow.get_first_child();
    while (c) { const n = c.get_next_sibling(); chipRow.remove(c); c = n; }

    if (recording) {
        chipRow.append(label("AWAITING INPUT…", "awaiting"));
        return;
    }
    const parts = cfg.hotkey.split("+").map(s => s.trim()).filter(Boolean);
    parts.forEach((p, i) => {
        if (i) chipRow.append(label("+", "chip-plus"));
        chipRow.append(label(p.replace(/^KEY_/, "").toLowerCase(), "chip"));
    });
}

function stopRecording() {
    recording = false;
    setBtn.set_label("[ set ]");
    setBtn.remove_css_class("recording");
    renderChips();
}

/* Writes [hotkey] only. The daemon owns the actual grab -- on Wayland a real
 * global grab lives in the compositor, so this just records the combo. */
function captureCombo(keyval, state) {
    if (keyval === Gdk.KEY_Escape) { stopRecording(); return; }

    const name = Gdk.keyval_name(keyval);
    if (!name) return;
    /* A bare modifier press starts a chord; it is not the chord itself. */
    if (/^(Control|Alt|Shift|Super|Meta)_[LR]$/.test(name)) return;

    const parts = MODS.filter(([m]) => (state & m) !== 0).map(([, n]) => n);
    parts.push("KEY_" + name.toUpperCase());
    cfg.hotkey = parts.join("+");

    stopRecording();
    persist();
}

/* ---- api key --------------------------------------------------------- */

/* The key is required, so this has to be enterable, not just readable. GTK's
 * own visibility flag does the masking -- rendering bullets by hand would mean
 * holding the real value somewhere else and writing it back on every edit. */
function renderKey() {
    keyEntry.set_visibility(keyShown);
    keyToggle.set_label(keyShown ? "[hide]" : "[show]");

    const fromEnv = envKey.length > 0;
    keyEntry.set_sensitive(!fromEnv);
    keyEntry.set_placeholder_text(fromEnv ? "set by OPENAI_API_KEY" : "sk-…");
    /* An empty key is the one state where nothing works at all, so it reads as
     * a prompt rather than as an ordinary blank field. */
    if (!fromEnv && !(cfg.api_key ?? "").length) keyEntry.add_css_class("needed");
    else keyEntry.remove_css_class("needed");
}

/* ---- live feed ------------------------------------------------------- */

function pushLine(text) {
    const ts = GLib.DateTime.new_now_local().format("%H:%M:%S");
    const row = box("h", 8, null, [label(ts, "feed-ts")]);
    row.append(label(text, "feed-text", {
        hexpand: true, wrap: true, wrap_mode: Pango.WrapMode.WORD_CHAR,
    }));

    /* The blinking cursor always trails the newest line. */
    const parent = cursor.get_parent();
    if (parent) parent.remove(cursor);
    row.append(cursor);

    feedBox.append(row);
    feedRows.push(row);
    while (feedRows.length > MAX_FEED) feedBox.remove(feedRows.shift());
}

/* ---- UI -------------------------------------------------------------- */

function buildLeft() {
    statusDot = label("●", "status-dot");
    statusTxt = label("active", null);

    const header = box("v", 2, null, [
        box("h", 0, null, [
            label("whisprd", "host"), label("@linux", "host-domain"),
            label("  ·  ", "host-sep"), statusDot, label(" ", null), statusTxt,
            label("  ·  ", "host-sep"), label("pipewire", null),
        ]),
        /* The one piece of information the old title bar carried: which file
         * these panels are actually editing. */
        label(Config.path().replace(GLib.get_home_dir(), "~"), "cfg-path"),
    ]);

    /* hotkey */
    chipRow = box("h", 5, null);
    setBtn = new Gtk.Button({ label: "[ set ]", css_classes: ["btn"] });
    setBtn.connect("clicked", () => {
        recording = !recording;
        setBtn.set_label(recording ? "[esc]" : "[ set ]");
        if (recording) setBtn.add_css_class("recording");
        else setBtn.remove_css_class("recording");
        renderChips();
    });
    renderChips();

    const hotkeyRow = box("h", 0, null, [chipRow]);
    hotkeyRow.append(spacer({ hexpand: true }));
    hotkeyRow.append(setBtn);

    /* microphone + meter */
    meter = new Gtk.DrawingArea({ height_request: 22, hexpand: true });
    meter.set_draw_func((area, cr, w, h) => {
        const gap = 2, bw = Math.max(1, (w - gap * (BARS - 1)) / BARS);
        cr.setSourceRGB(1.0, 75 / 255, 75 / 255);          /* #ff4b4b */
        for (let i = 0; i < BARS; i++) {
            const v = Math.min(1, meterValues[i] ?? 0);
            const bh = Math.max(1, v * h);
            cr.rectangle(i * (bw + gap), h - bh, bw, bh);
        }
        cr.fill();
    });

    srcList = Audio.sources();
    const srcIdx = Math.max(0, srcList.findIndex(s => s.name === cfg.source));
    const micSel = select(srcList.map(s => s.description), srcIdx, i => {
        cfg.source = srcList[i].name;
        /* The meter must follow the selection, or it reports a device the
         * daemon is no longer going to use. */
        if (levelMeter) levelMeter.setSource(cfg.source);
        persist();
    });

    /* model */
    const mdIdx = Math.max(0, MODELS.findIndex(m => m.model === cfg.model));
    const mdSel = select(MODELS.map(m => m.text), mdIdx, i => {
        cfg.model = MODELS[i].model;
        persist();
    });

    /* api_key */
    keyEntry = new Gtk.Entry({
        text: envKey.length ? "" : (cfg.api_key ?? ""),
        visibility: false,
        hexpand: true,
        has_frame: false,
        css_classes: ["key-entry"],
    });
    /* Saved on Enter and on focus-out, never per keystroke: persist() SIGHUPs
     * the daemon, which tears the capture stack down and builds it back up --
     * once per character would restart it fifty times for one paste. */
    const commitKey = () => {
        /* When the environment supplies the key the entry is blank and
         * insensitive. Committing that blank would erase the key in the file
         * -- which is still the value the daemon falls back to. */
        if (envKey.length) return;
        const v = keyEntry.get_text().trim();
        if (v === (cfg.api_key ?? "")) return;
        cfg.api_key = v;
        persist();
        renderKey();
    };
    keyEntry.connect("activate", commitKey);
    const keyFocus = new Gtk.EventControllerFocus();
    keyFocus.connect("leave", commitKey);
    keyEntry.add_controller(keyFocus);

    keyToggle = new Gtk.Button({ label: "[show]", css_classes: ["link"], has_frame: false });
    keyToggle.connect("clicked", () => { keyShown = !keyShown; renderKey(); });
    renderKey();

    /* kb_layout */
    const lyIdx = Math.max(0, LAYOUTS.findIndex(
        l => l.layout === cfg.layout && l.variant === (cfg.variant ?? "")));
    const lySel = select(LAYOUTS.map(l => l.text), lyIdx, i => {
        cfg.layout = LAYOUTS[i].layout;
        cfg.variant = LAYOUTS[i].variant;
        persist();
    });

    const settings = box("v", 11, "settings", [
        header,
        field("─ hotkey ─", hotkeyRow),
        field("─ microphone ─", box("v", 7, null, [micSel, meter])),
        field("─ model ─", mdSel),
        field("─ api_key ─", box("h", 0, null, [keyEntry, keyToggle])),
        field("─ kb_layout ─", lySel),
    ]);
    settings.set_size_request(LEFT_W, -1);
    return settings;
}

/* One row: time · name + snippet · words · delete (revealed on hover).
 *
 * The delete button is built hidden rather than transparent, so it cannot be
 * clicked while invisible. */
function sessionRow(entry, index) {
    const del = new Gtk.Button({
        label: "✕",
        css_classes: ["del"],
        has_frame: false,
        visible: false,
        tooltip_text: "move to trash",
        valign: Gtk.Align.CENTER,
    });
    del.connect("clicked", () => {
        const result = Sessions.remove(entry.path);
        if (result === "failed") {
            del.add_css_class("failed");
            return;
        }
        refreshSessions();
    });

    const row = box("h", 10, "sess-row", [
        label(entry.when, "sess-time", { width_request: 46 }),
        label(`${entry.name}  ${entry.snippet}`, "sess-name",
              { hexpand: true, ellipsize: Pango.EllipsizeMode.END }),
        label(entry.words, "sess-words"),
        del,
    ]);

    const motion = new Gtk.EventControllerMotion();
    motion.connect("enter", () => {
        del.set_visible(true);
        selected = index;
        markSelection();
    });
    motion.connect("leave", () => del.set_visible(false));
    row.add_controller(motion);

    const click = new Gtk.GestureClick();
    click.connect("pressed", (_g, n) => {
        selected = index;
        markSelection();
        if (n >= 2) Sessions.open(entry.path);
    });
    row.add_controller(click);

    return row;
}

function markSelection() {
    sessRows.forEach(({ widget }, i) => {
        if (i === selected) widget.add_css_class("selected");
        else widget.remove_css_class("selected");
    });
}

function refreshSessions() {
    let c = sessBox.get_first_child();
    while (c) { const n = c.get_next_sibling(); sessBox.remove(c); c = n; }

    const entries = Sessions.list();
    sessRows = entries.slice(0, 6).map((entry, i) => {
        const widget = sessionRow(entry, i);
        sessBox.append(widget);
        return { entry, widget };
    });

    countLabel.set_label(`${entries.length} file${entries.length === 1 ? "" : "s"}`);

    /* An empty list is ambiguous -- nothing dictated yet, or recording never
     * switched on? Say which. */
    const recording = Config.load().history === "on";
    emptyNote.set_visible(entries.length === 0);
    emptyNote.set_label(recording
        ? "no transcripts yet — hold your hotkey and speak"
        : "recording is off — set  history = on  in config.ini");

    if (selected >= sessRows.length) selected = Math.max(0, sessRows.length - 1);
    markSelection();
}

function buildRight() {
    countLabel = label("0 files", "history-count");
    const subhead = box("h", 8, null, [
        label("transcriptions/", "history-title"),
        label("grep ▏", "history-grep"),
    ]);
    subhead.append(spacer({ hexpand: true }));
    subhead.append(countLabel);

    feedBox = box("v", 2, null, [label("$ tail -f session.log", "feed-cmd")]);
    cursor = label("█", "feed-cursor");

    sessBox = box("v", 3, null);
    emptyNote = label("", "empty-note");

    return box("v", 9, "history", [
        subhead,
        new Gtk.Box({ css_classes: ["history-rule"] }),
        feedBox,
        spacer({ vexpand: true }),
        label("──────── recent sessions ────────", "divider",
              { xalign: 0.5, hexpand: true }),
        sessBox,
        emptyNote,
    ]);
}

function buildFooter() {
    const fkey = (k, t) => box("h", 4, null, [label(k, "fkey"), label(t, null)]);
    const footer = box("h", 14, "footer", [
        fkey("⏎", "open"), fkey("/", "search"), fkey("^E", "export"),
    ]);
    footer.append(spacer({ hexpand: true }));
    footer.append(fkey("^X", "close"));
    return footer;
}

/* ---- runtime wiring -------------------------------------------------- */

function setStatus(active) {
    statusTxt.set_label(active ? "active" : "inactive");
    if (active) statusDot.remove_css_class("idle");
    else statusDot.add_css_class("idle");
}

function startRuntime() {
    setStatus(Daemon.isActive());
    refreshSessions();

    feed = new Daemon.Feed(line => {
        pushLine(line);
        /* A new utterance either extends the current session file or starts a
         * new one; either way the list and word counts are now stale. */
        if (sessionRefresh) GLib.Source.remove(sessionRefresh);
        sessionRefresh = GLib.timeout_add_seconds(GLib.PRIORITY_LOW, 2, () => {
            sessionRefresh = 0;
            refreshSessions();
            return GLib.SOURCE_REMOVE;
        });
    });
    feed.start();

    levelMeter = new Audio.Meter(BARS, vals => {
        meterValues = vals;
        meter.queue_draw();
    });
    levelMeter.start(cfg.source);

    /* 1.1s step blink, per the spec. */
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 550, () => {
        if (cursor.has_css_class("blink-off")) cursor.remove_css_class("blink-off");
        else cursor.add_css_class("blink-off");
        return GLib.SOURCE_CONTINUE;
    });

    GLib.timeout_add_seconds(GLib.PRIORITY_LOW, 3, () => {
        setStatus(Daemon.isActive());
        return GLib.SOURCE_CONTINUE;
    });
}

/* ---- application ----------------------------------------------------- */

const app = new Gtk.Application({
    application_id: "dev.whisprd.Menu",
    flags: Gio.ApplicationFlags.FLAGS_NONE,
});

app.connect("activate", () => {
    const css = new Gtk.CssProvider();
    css.load_from_path(GLib.build_filenamev([HERE, "style.css"]));
    Gtk.StyleContext.add_provider_for_display(
        Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION);

    win = new Gtk.ApplicationWindow({
        application: app,
        css_classes: ["whisprd-menu"],
        /* Without this the compositor shows "gjs" in alt-tab and window lists. */
        title: "whisprd",
        default_width: W, default_height: H,
        resizable: false,
        /* The design draws its own 1px border and 8px radius; server-side
         * decorations would sit outside them and clash. */
        decorated: false,
    });

    const content = box("v", 0, "panel", [
        box("h", 0, null, [buildLeft(), buildRight()]),
        buildFooter(),
    ]);

    /* Undecorated windows have no titlebar to drag by. A WindowHandle makes
     * any part of the panel that is not itself interactive a drag surface, so
     * it moves like a normal window without adding chrome back. */
    win.set_child(new Gtk.WindowHandle({ child: content }));

    if (LayerShell) {
        LayerShell.init_for_window(win);
        LayerShell.set_layer(win, LayerShell.Layer.OVERLAY);
        LayerShell.set_keyboard_mode(win, LayerShell.KeyboardMode.EXCLUSIVE);
        LayerShell.set_namespace(win, "whisprd-menu");
        /* No anchors == centred on the output. */
    }

    const keys = new Gtk.EventControllerKey();
    keys.connect("key-pressed", (_c, keyval, _code, state) => {
        if (recording) { captureCombo(keyval, state); return true; }

        const ctrl = (state & Gdk.ModifierType.CONTROL_MASK) !== 0;
        const cur = sessRows[selected];

        if (keyval === Gdk.KEY_Escape || (ctrl && keyval === Gdk.KEY_x)) {
            app.quit();
            return true;
        }
        if (keyval === Gdk.KEY_Down || keyval === Gdk.KEY_j) {
            selected = Math.min(selected + 1, sessRows.length - 1);
            markSelection();
            return true;
        }
        if (keyval === Gdk.KEY_Up || keyval === Gdk.KEY_k) {
            selected = Math.max(selected - 1, 0);
            markSelection();
            return true;
        }
        if (keyval === Gdk.KEY_Return || keyval === Gdk.KEY_KP_Enter) {
            if (cur) Sessions.open(cur.entry.path);
            return true;
        }
        if (ctrl && keyval === Gdk.KEY_e) {
            /* "Export" with nowhere to export to is just the file manager:
             * the sessions are already plain .txt on disk. */
            Sessions.open(Sessions.dir());
            return true;
        }
        if (keyval === Gdk.KEY_Delete && cur) {
            if (Sessions.remove(cur.entry.path) !== "failed") refreshSessions();
            return true;
        }
        return false;
    });
    win.add_controller(keys);

    win.present();
    startRuntime();
});

app.connect("shutdown", () => {
    if (feed) feed.stop();
    if (levelMeter) levelMeter.stop();
});

app.run(null);
