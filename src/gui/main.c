/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* whisprd-gui -- settings window.
 *
 * Deliberately a separate binary: the daemon keeps zero GUI dependencies and
 * still runs headless. There is no IPC. This edits config.ini, and the two
 * genuinely useful live features need no daemon:
 *
 *   - the level meter opens its own capture stream, so you choose a
 *     microphone by watching the bar move rather than guessing from a name;
 *   - "Test injection" shells out to `whisprd --say`, so the injection
 *     backend can be verified without speaking. */

#include "../audio.h"
#include "../config.h"

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWindow    *win;
    GtkWidget    *hotkey, *endpoint, *model, *api_key;
    GtkWidget    *layout, *variant, *paste_chord;
    GtkWidget    *backend_dd, *source_dd;
    GtkWidget    *meter, *meter_label, *status;

    audio_source *sources;
    size_t        n_sources;
    char          cfg_path[512];
} app_state;

/* ---- live level meter -------------------------------------------------- */

static pthread_t   meter_thread;
static atomic_bool meter_running;
static atomic_int  meter_peak;
static pthread_mutex_t meter_lock = PTHREAD_MUTEX_INITIALIZER;
static char        meter_source[256];
static atomic_int  meter_generation;

static void *meter_loop(void *arg)
{
    (void)arg;
    while (atomic_load(&meter_running)) {
        int gen = atomic_load(&meter_generation);

        char src[256];
        pthread_mutex_lock(&meter_lock);
        snprintf(src, sizeof(src), "%s", meter_source);
        pthread_mutex_unlock(&meter_lock);

        if (!src[0]) {
            g_usleep(100000);
            continue;
        }

        /* Short samples so a source change is picked up promptly. */
        int peak = audio_measure_peak(src, 120);
        if (atomic_load(&meter_generation) != gen)
            continue;                       /* selection changed mid-read */
        atomic_store(&meter_peak, peak < 0 ? -1 : peak);
    }
    return NULL;
}

static gboolean meter_tick(gpointer data)
{
    app_state *st = data;
    int peak = atomic_load(&meter_peak);

    if (peak < 0) {
        gtk_level_bar_set_value(GTK_LEVEL_BAR(st->meter), 0.0);
        gtk_label_set_text(GTK_LABEL(st->meter_label), "source unavailable");
        return G_SOURCE_CONTINUE;
    }

    double frac = peak / 32767.0;
    /* Speech sits low on a linear scale; a sqrt curve makes the bar readable. */
    gtk_level_bar_set_value(GTK_LEVEL_BAR(st->meter), CLAMP(sqrt(frac), 0.0, 1.0));

    char buf[64];
    double pct = peak / 327.68;
    if (pct < 2.0) {
        snprintf(buf, sizeof(buf), "%.1f%%  — too quiet to transcribe", pct);
        gtk_widget_add_css_class(st->meter, "low");
    } else {
        snprintf(buf, sizeof(buf), "%.1f%%", pct);
        gtk_widget_remove_css_class(st->meter, "low");
    }
    gtk_label_set_text(GTK_LABEL(st->meter_label), buf);
    return G_SOURCE_CONTINUE;
}

static void meter_set_source(const char *name)
{
    pthread_mutex_lock(&meter_lock);
    snprintf(meter_source, sizeof(meter_source), "%s", name ? name : "");
    pthread_mutex_unlock(&meter_lock);
    atomic_fetch_add(&meter_generation, 1);
    atomic_store(&meter_peak, 0);
}

/* ---- small widget helpers ---------------------------------------------- */

static void add_class(GtkWidget *w, const char *cls)
{
    gtk_widget_add_css_class(w, cls);
}

static GtkWidget *card(const char *title)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    add_class(box, "card");

    GtkWidget *lbl = gtk_label_new(title);
    add_class(lbl, "section");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), lbl);
    return box;
}

/* One labelled row: caption on the left, control on the right. */
static GtkWidget *row(GtkWidget *parent, const char *label, GtkWidget *control,
                      const char *hint)
{
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *cap = gtk_label_new(label);
    add_class(cap, "rowlabel");
    gtk_widget_set_halign(cap, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(left), cap);
    if (hint) {
        GtkWidget *h = gtk_label_new(hint);
        add_class(h, "hint");
        gtk_widget_set_halign(h, GTK_ALIGN_START);
        gtk_label_set_wrap(GTK_LABEL(h), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(h), 40);
        gtk_box_append(GTK_BOX(left), h);
    }
    gtk_widget_set_size_request(left, 230, -1);
    gtk_box_append(GTK_BOX(line), left);

    gtk_widget_set_hexpand(control, TRUE);
    gtk_widget_set_valign(control, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(line), control);

    gtk_box_append(GTK_BOX(parent), line);
    return line;
}

static GtkWidget *entry(const char *text, bool mono)
{
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), text ? text : "");
    if (mono)
        add_class(e, "mono");
    return e;
}

static const char *entry_text(GtkWidget *e)
{
    return gtk_editable_get_text(GTK_EDITABLE(e));
}

static void set_status(app_state *st, const char *msg, const char *cls)
{
    gtk_label_set_text(GTK_LABEL(st->status), msg);
    gtk_widget_remove_css_class(st->status, "ok");
    gtk_widget_remove_css_class(st->status, "warn");
    if (cls)
        add_class(st->status, cls);
}

/* ---- config load / save ------------------------------------------------ */

static void config_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%s/whisprd/config.ini", xdg);
    else
        snprintf(out, n, "%s/.config/whisprd/config.ini", g_get_home_dir());
}

static const char *backend_ids[] = { "auto", "wlr-vk", "uinput", "clipboard" };

static void on_save(GtkButton *btn, gpointer data)
{
    app_state *st = data;

    guint bi = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->backend_dd));
    const char *backend = bi < G_N_ELEMENTS(backend_ids) ? backend_ids[bi] : "auto";

    guint si = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->source_dd));
    const char *source = "";
    if (si > 0 && si - 1 < st->n_sources)      /* index 0 is "system default" */
        source = st->sources[si - 1].name;

    char dir[512];
    snprintf(dir, sizeof(dir), "%s", st->cfg_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        g_mkdir_with_parents(dir, 0700);
    }

    FILE *f = fopen(st->cfg_path, "w");
    if (!f) {
        set_status(st, "could not write the config file", "warn");
        return;
    }

    fprintf(f,
        "# whisprd configuration -- written by whisprd-gui\n\n"
        "# --- hotkey ---\n"
        "hotkey = %s\n\n"
        "# --- capture ---\n"
        "# empty = system default. List devices with: whisprd --list-sources\n"
        "source = %s\n\n"
        "# --- inference endpoint (cloud vs local is JUST this URL) ---\n"
        "endpoint_url = %s\n"
        "model        = %s\n"
        "api_key      = %s\n\n"
        "# --- injection ---\n"
        "# backend = auto | wlr-vk | uinput | clipboard\n"
        "backend = %s\n"
        "layout  = %s\n"
        "variant = %s\n"
        "paste_chord = %s\n",
        entry_text(st->hotkey), source,
        entry_text(st->endpoint), entry_text(st->model), entry_text(st->api_key),
        backend, entry_text(st->layout), entry_text(st->variant),
        entry_text(st->paste_chord));
    fclose(f);

    /* The file holds an API key, so keep it to the owner. */
    g_chmod(st->cfg_path, 0600);

    char msg[600];
    snprintf(msg, sizeof(msg), "Saved to %s — restart whisprd to apply.",
             st->cfg_path);
    set_status(st, msg, "ok");
}

/* ---- test injection ---------------------------------------------------- */

static void on_test(GtkButton *btn, gpointer data)
{
    app_state *st = data;

    /* Reuses the daemon's own code path rather than reimplementing injection,
     * so what this proves is exactly what the daemon will do. */
    char *argv[] = { "whisprd", "--say", "whisprd injection test", NULL };
    GError *err = NULL;
    gboolean ok = g_spawn_async(NULL, argv, NULL,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                                NULL, NULL, NULL, &err);
    if (!ok) {
        char msg[512];
        snprintf(msg, sizeof(msg), "could not run whisprd: %s",
                 err ? err->message : "unknown error");
        set_status(st, msg, "warn");
        g_clear_error(&err);
        return;
    }
    set_status(st, "Sent — focus a text field within a moment to catch it.", "ok");
}

/* ---- window ------------------------------------------------------------ */

static void on_source_changed(GtkDropDown *dd, GParamSpec *ps, gpointer data)
{
    app_state *st = data;
    guint i = gtk_drop_down_get_selected(dd);
    meter_set_source(i > 0 && i - 1 < st->n_sources ? st->sources[i - 1].name : "");
}

static void build_ui(GtkApplication *app, gpointer data)
{
    app_state *st = data;

    config cfg;
    config_path(st->cfg_path, sizeof(st->cfg_path));
    config_load(&cfg, st->cfg_path);

    GtkWidget *win = gtk_application_window_new(app);
    st->win = GTK_WINDOW(win);
    gtk_window_set_title(st->win, "whisprd");
    gtk_window_set_default_size(st->win, 760, 820);
    add_class(win, "whisprd");

    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_margin_top(outer, 28);
    gtk_widget_set_margin_bottom(outer, 28);
    gtk_widget_set_margin_start(outer, 28);
    gtk_widget_set_margin_end(outer, 28);

    /* Title */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *t = gtk_label_new("whisprd");
    add_class(t, "apptitle");
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    GtkWidget *sub = gtk_label_new("hold-to-talk voice transcription");
    add_class(sub, "subtitle");
    gtk_widget_set_halign(sub, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(head), t);
    gtk_box_append(GTK_BOX(head), sub);
    gtk_box_append(GTK_BOX(outer), head);

    /* --- hotkey --- */
    char hk[128];
    config_hotkey_desc(&cfg, hk, sizeof(hk));
    GtkWidget *c1 = card("HOTKEY");
    st->hotkey = entry(hk, true);
    row(c1, "Hold to talk", st->hotkey,
        "evdev key name, or MOD+KEY. The key is observed, not grabbed, so it "
        "still reaches your applications.");
    gtk_box_append(GTK_BOX(outer), c1);

    /* --- microphone --- */
    GtkWidget *c2 = card("MICROPHONE");
    st->sources = audio_enumerate_sources(&st->n_sources);

    GtkStringList *names = gtk_string_list_new(NULL);
    gtk_string_list_append(names, "System default");
    guint selected = 0;
    for (size_t i = 0; i < st->n_sources; i++) {
        char label[600];
        snprintf(label, sizeof(label), "%s%s", st->sources[i].desc,
                 st->sources[i].monitor ? "  (monitor — not a microphone)" : "");
        gtk_string_list_append(names, label);
        if (cfg.source[0] && strcmp(cfg.source, st->sources[i].name) == 0)
            selected = (guint)i + 1;
    }
    st->source_dd = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(st->source_dd), selected);
    row(c2, "Capture source", st->source_dd,
        "Speak and watch the meter. If it does not move, this is the wrong "
        "device — that is what makes transcripts come back as nonsense.");

    st->meter = gtk_level_bar_new_for_interval(0.0, 1.0);
    gtk_widget_set_hexpand(st->meter, TRUE);
    st->meter_label = gtk_label_new("—");
    add_class(st->meter_label, "hint");
    gtk_widget_set_halign(st->meter_label, GTK_ALIGN_START);
    GtkWidget *mbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(mbox), st->meter);
    gtk_box_append(GTK_BOX(mbox), st->meter_label);
    row(c2, "Input level", mbox, NULL);
    gtk_box_append(GTK_BOX(outer), c2);

    /* --- transcription --- */
    GtkWidget *c3 = card("TRANSCRIPTION");
    st->endpoint = entry(cfg.endpoint_url, true);
    row(c3, "Endpoint", st->endpoint,
        "Local keeps audio on this machine: http://127.0.0.1:8080/v1 — "
        "cloud is https://api.openai.com/v1");
    st->model = entry(cfg.model, true);
    row(c3, "Model", st->model, NULL);
    st->api_key = entry(cfg.api_key, true);
    gtk_entry_set_visibility(GTK_ENTRY(st->api_key), FALSE);
    row(c3, "API key", st->api_key,
        "Only needed for cloud endpoints. Stored in the config file, "
        "readable only by you.");
    gtk_box_append(GTK_BOX(outer), c3);

    /* --- injection --- */
    GtkWidget *c4 = card("INJECTION");
    GtkStringList *bl = gtk_string_list_new(NULL);
    gtk_string_list_append(bl, "Automatic");
    gtk_string_list_append(bl, "Wayland virtual keyboard (wlroots)");
    gtk_string_list_append(bl, "uinput, declared layout (GNOME/KDE)");
    gtk_string_list_append(bl, "Clipboard + paste chord (universal)");
    st->backend_dd = gtk_drop_down_new(G_LIST_MODEL(bl), NULL);
    for (guint i = 0; i < G_N_ELEMENTS(backend_ids); i++)
        if (strcmp(cfg.backend, backend_ids[i]) == 0)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(st->backend_dd), i);
    row(c4, "Backend", st->backend_dd,
        "Automatic prefers direct typing and falls back to the clipboard.");

    st->layout = entry(cfg.layout, true);
    row(c4, "Keyboard layout", st->layout,
        "Used by the uinput backend. Must match your active layout, or output "
        "is scrambled.");
    st->variant = entry(cfg.variant, true);
    row(c4, "Layout variant", st->variant, "Optional, e.g. intl or nodeadkeys.");

    char pc[128];
    config_paste_chord_desc(&cfg, pc, sizeof(pc));
    st->paste_chord = entry(pc, true);
    row(c4, "Paste chord", st->paste_chord,
        "Clipboard backend only. Terminals usually need ctrl+shift+v.");
    gtk_box_append(GTK_BOX(outer), c4);

    /* --- actions --- */
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *test = gtk_button_new_with_label("Test injection");
    g_signal_connect(test, "clicked", G_CALLBACK(on_test), st);
    GtkWidget *save = gtk_button_new_with_label("Save");
    add_class(save, "suggested");
    g_signal_connect(save, "clicked", G_CALLBACK(on_save), st);

    st->status = gtk_label_new("");
    add_class(st->status, "status");
    gtk_widget_set_halign(st->status, GTK_ALIGN_START);
    gtk_widget_set_hexpand(st->status, TRUE);
    gtk_label_set_wrap(GTK_LABEL(st->status), TRUE);

    gtk_box_append(GTK_BOX(actions), st->status);
    gtk_box_append(GTK_BOX(actions), test);
    gtk_box_append(GTK_BOX(actions), save);
    gtk_box_append(GTK_BOX(outer), actions);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), outer);
    gtk_window_set_child(st->win, scroll);

    /* Start metering whatever is selected. */
    g_signal_connect(st->source_dd, "notify::selected",
                     G_CALLBACK(on_source_changed), st);
    on_source_changed(GTK_DROP_DOWN(st->source_dd), NULL, st);

    atomic_store(&meter_running, true);
    pthread_create(&meter_thread, NULL, meter_loop, NULL);
    g_timeout_add(80, meter_tick, st);

    gtk_window_present(st->win);
}

static void load_css(GtkApplication *app, gpointer user)
{
    GtkCssProvider *p = gtk_css_provider_new();
    /* Installed path first, then the source tree, so it runs uninstalled. */
    const char *paths[] = {
        DATADIR "/whisprd/style.css",
        "src/gui/style.css",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(paths); i++) {
        if (g_file_test(paths[i], G_FILE_TEST_EXISTS)) {
            GFile *f = g_file_new_for_path(paths[i]);
            gtk_css_provider_load_from_file(p, f);
            g_object_unref(f);
            break;
        }
    }
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

int main(int argc, char **argv)
{
    app_state st = { 0 };

    GtkApplication *app = gtk_application_new("dev.whisprd.Settings",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "startup", G_CALLBACK(load_css), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(build_ui), &st);

    int rc = g_application_run(G_APPLICATION(app), argc, argv);

    atomic_store(&meter_running, false);
    pthread_join(meter_thread, NULL);
    free(st.sources);
    g_object_unref(app);
    return rc;
}
