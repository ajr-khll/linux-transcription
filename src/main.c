/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "audio.h"
#include "config.h"
#include "confwatch.h"
#include "cue.h"
#include "history.h"
#include "injector.h"
#include "input.h"
#include "live.h"
#include "log.h"
#include "polish.h"
#include "queue.h"
#include "screen.h"
#include "transcribe.h"

#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_CAP 4

static queue    *jobs;
static injector *inj;
static bool      print_only;

/* True when the worker itself drives the screen record, so it can put the raw
 * transcript up and then swap the cleaned one in. That is the cleanup path with
 * the preview off: the preview, when it is on, owns the screen instead. Needs a
 * backend that can erase and a transcript actually being typed, so it is off
 * for print-only and for the clipboard. */
static bool      worker_screen;

/* Runs on the input thread, so it must not block. */
static void on_hold(bool holding, void *user)
{
    (void)user;
    log_dbg("%s\n", holding ? "HOLD_START" : "HOLD_STOP");
    audio_set_capturing(holding);
    cue_play(holding ? CUE_START : CUE_STOP);
}

/* Puts `text` on screen, or takes the preview back when there is none.
 *
 * Every way an utterance can end comes through here, including the ways that
 * produce nothing: a rejected utterance, a failed decode, a transcript of
 * nothing but spaces. The live preview has usually typed something by then, and
 * the only way to be sure none of those paths leaves stale words behind is to
 * give them all the same exit. */
static void deliver(const char *text)
{
    if (live_active()) {
        live_commit(text);                  /* retracts the preview first */
        return;
    }
    if (worker_screen) {
        screen_render(text);                /* retracts a staged transcript */
        screen_release();
        return;
    }
    if (!print_only && *text && injector_send(inj, text, NULL) < 0) {
        log_err("injection failed\n");
        cue_play(CUE_ERROR);
    }
}

/* Delivers a real transcript, cleaning it up on the way when cleanup is on: the
 * raw text goes on screen first, then the model's corrected version replaces it
 * when it arrives. Text is never withheld waiting on the model -- a slow or dead
 * endpoint just leaves the raw transcript standing.
 *
 * The swap needs a screen to erase into. With the preview on, its thread owns
 * that; with the preview off but an erasable backend, the worker does
 * (worker_screen). Anywhere else -- the clipboard, print-only -- there is no
 * swap, so the cleaned text is simply what gets delivered, once. */
static void deliver_transcript(const char *raw)
{
    unsigned gen = 0;
    if (live_active())
        gen = live_stage(raw);              /* raw up now, kept for the swap */
    else if (worker_screen)
        screen_render(raw);

    char *clean = polish_enabled() ? polish_text(raw) : NULL;
    const char *final = clean ? clean : raw;
    if (clean && strcmp(clean, raw) != 0)
        log_info("cleaned: %s\n", clean);

    if (live_active()) {
        live_commit_staged(final, gen);     /* dropped if a newer utterance began */
    } else if (worker_screen) {
        screen_render(final);
        screen_release();
    } else if (!print_only && *final && injector_send(inj, final, NULL) < 0) {
        log_err("injection failed\n");
        cue_play(CUE_ERROR);
    }

    /* Recorded after the swap so the file keeps what the user was left with. */
    history_append(final);
    free(clean);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pcm_buffer *b = queue_pop(jobs);
        if (!b)
            return NULL;                /* queue closed: shutting down */

        /* The capture thread already decided there was nothing here. It could
         * not retract the preview itself without blocking the audio mainloop,
         * so it said so and left it to us. */
        if (b->rejected) {
            pcm_buffer_free(b);
            deliver("");
            continue;
        }

        char *text = transcribe_pcm(b->samples, b->n_samples);
        pcm_buffer_free(b);
        if (!text) {
            cue_play(CUE_ERROR);            /* network or endpoint failure */
            deliver("");
            continue;
        }

        /* Leading space is common from Whisper and reads as a typo. */
        char *start = text;
        while (*start == ' ')
            start++;

        if (*start) {
            log_info("transcript: %s\n", start);
            deliver_transcript(start);
        } else {
            cue_play(CUE_ERROR);            /* nothing was recognised */
            deliver("");                    /* retract any preview */
        }
        free(text);
    }
}

/* Set from the SIGHUP handler; read by main to decide whether to re-enter the
 * setup loop or exit. sig_atomic_t is the only type safe to touch here. */
static volatile sig_atomic_t reload_requested;

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        reload_requested = 1;
    input_stop();                       /* write() is async-signal-safe */
}

/* Runs on the config watcher's thread when config.ini changes on disk. Does
 * exactly what the SIGHUP handler does, and for the same reason: the work of
 * reloading belongs on the main thread, which is sitting in epoll_wait until
 * input_stop() writes to the eventfd that wakes it. */
static void on_config_changed(void)
{
    log_info("config changed on disk, reloading\n");
    reload_requested = 1;
    input_stop();
}

static void usage(void)
{
    fprintf(stderr,
        "usage: scribe [-c CONFIG] [-p] [-v]\n"
        "       scribe --say TEXT      inject TEXT and exit (tests injection)\n"
        "       scribe --list-sources  show capture devices and their levels\n"
        "\n"
        "  -c FILE  config file (default $XDG_CONFIG_HOME/scribe/config.ini)\n"
        "  -p       print transcripts to stdout instead of injecting them\n"
        "  -v       verbose logging\n"
        "  -V       print version and exit\n"
        "  -h       this help\n");
}

/* Injects TEXT and exits. Needs no microphone and no endpoint, so it is the
 * fastest way to tell whether the chosen injection backend works at all. */
static int run_say(const config *cfg, const char *text)
{
    injector *i = injector_init(cfg);
    if (!i)
        return 1;
    int rc = injector_send(i, text, NULL) < 0 ? 1 : 0;
    injector_destroy(i);
    return rc;
}

int main(int argc, char **argv)
{
    const char *cfg_path = NULL;
    const char *say_text = NULL;
    bool list_sources = false;

    static const struct option longopts[] = {
        { "config",       required_argument, NULL, 'c' },
        { "print",        no_argument,       NULL, 'p' },
        { "verbose",      no_argument,       NULL, 'v' },
        { "version",      no_argument,       NULL, 'V' },
        { "help",         no_argument,       NULL, 'h' },
        { "say",          required_argument, NULL, 's' },
        { "list-sources", no_argument,       NULL, 'L' },
        { 0, 0, 0, 0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:pvVhs:L", longopts, NULL)) != -1) {
        switch (opt) {
        case 'c': cfg_path = optarg; break;
        case 'p': print_only = true; break;
        case 'v': log_verbose = 1; break;
        case 's': say_text = optarg; break;
        case 'L': list_sources = true; break;
        case 'V': printf("scribe %s\n", SCRIBE_VERSION); return 0;
        default:  usage(); return opt == 'h' ? 0 : 2;
        }
    }

    /* The clipboard backend writes the transcript into a pipe to wl-copy. If
     * that helper is missing, the child exits before the write lands and the
     * pipe raises SIGPIPE, whose default action would kill the daemon outright
     * -- and racily, so it would not reproduce reliably. Ignore it here and let
     * write() report EPIPE to the code that can explain it. */
    signal(SIGPIPE, SIG_IGN);

    config cfg;
    if (config_load(&cfg, cfg_path) < 0)
        return 1;

    /* Resolved once, here, rather than recomputed by the watcher: the two must
     * agree on which file matters, and only config.c knows where the default
     * lives. */
    char config_path[512];
    config_resolve_path(config_path, sizeof(config_path), cfg_path);

    /* Both of these are diagnostics: they must work before the daemon does. */
    if (list_sources)
        return audio_list_sources() < 0 ? 1 : 0;
    if (say_text)
        return run_say(&cfg, say_text);

    /* SIGHUP re-reads the config. Every subsystem captures its settings at
     * init time -- the hotkey lives in the evdev loop, the endpoint in the
     * curl handle, the source in the capture stream -- so applying a change
     * means tearing the stack down and standing it back up. That is what this
     * loop does: same process, same uinput device, fresh config. */
    int rc = 1;
    bool reloading;

    do {
        pthread_t worker_thread;
        bool worker_started = false;

        reload_requested = 0;
        reloading = false;
        worker_screen = false;

        /* Logged before anything can fail, so a permissions error still tells
         * the user which key they were meant to be holding. */
        char hotkey[128];
        config_hotkey_desc(&cfg, hotkey, sizeof(hotkey));
        log_info("hotkey: %s\n", hotkey);

        jobs = queue_create(QUEUE_CAP);
        if (!jobs)
            goto out;

        if (transcribe_init(&cfg) < 0)
            goto out;

        /* Refuses at startup, like the OpenAI key check, when cleanup is on but
         * its endpoint is not local. A no-op when cleanup is off. */
        if (polish_init(&cfg) < 0)
            goto out;

        /* A history directory we cannot create is worth reporting, but not
         * worth refusing to transcribe over. */
        history_init(&cfg);

        cue_init(&cfg);

        /* The injector claims /dev/uinput before input_init enumerates
         * devices, so our own virtual keyboard already exists and gets
         * skipped there. */
        if (!print_only) {
            inj = injector_init(&cfg);
            if (!inj)
                goto out;
            /* After the injector, because it asks whether that backend can
             * erase; before audio_init, because the capture callback starts
             * calling live_feed the moment the stream opens. */
            live_init(&cfg, inj);

            /* The preview owns the screen when it runs. When it does not but
             * cleanup does, and the backend can erase, the worker owns it
             * instead, so the raw transcript can be swapped for the cleaned one.
             * live_init already attaches the screen in the preview case. */
            worker_screen = !live_active() && polish_enabled() &&
                            injector_can_erase(inj);
            if (worker_screen)
                screen_attach(inj);
        }

        if (input_init(&cfg, on_hold, NULL) < 0)
            goto out;

        if (audio_init(&cfg, jobs) < 0)
            goto out;

        /* Started last, so a change arriving mid-startup cannot ask for a
         * reload of a stack that is not up yet. Failing is not fatal: SIGHUP
         * still works, and confwatch_init says so. */
        confwatch_init(config_path, on_config_changed);

        if (pthread_create(&worker_thread, NULL, worker, NULL) != 0) {
            log_err("cannot start worker thread\n");
            goto out;
        }
        worker_started = true;

        struct sigaction sa = { .sa_handler = on_signal };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);

        log_info("ready; hold %s to talk\n", hotkey);
        rc = input_run() < 0 ? 1 : 0;

        /* input_run returns for either reason; the flag says which. Two things
         * set that flag now -- a SIGHUP and the file watcher -- and the watcher
         * has already said so, so naming SIGHUP here would be a guess and half
         * the time a wrong one. */
        reloading = reload_requested != 0;
        log_info("%s\n", reloading ? "reloading configuration"
                                   : "shutting down");

    out:
        /* First, so a save landing during teardown cannot set the reload flag
         * after we have read it. */
        confwatch_shutdown();
        audio_shutdown();
        if (jobs)
            queue_close(jobs);
        if (worker_started)
            pthread_join(worker_thread, NULL);
        /* After the worker, which is the only thing that commits, and after
         * audio_shutdown, which is the only thing that feeds. Before the
         * injector is destroyed, because taking a leftover preview back is the
         * last thing it does. */
        live_shutdown();
        /* When the worker owned the screen, live_shutdown left it alone; take
         * back any transcript still standing before the injector goes away. A
         * no-op when the preview owned it, since live_shutdown already detached. */
        if (worker_screen)
            screen_detach();
        input_shutdown();
        injector_destroy(inj);
        history_shutdown();
        polish_shutdown();
        transcribe_shutdown();
        queue_destroy(jobs);
        jobs = NULL;
        inj = NULL;

        /* Reload into a scratch copy and only adopt it if it parsed. A bad
         * config would otherwise swap every setting the daemon is running with
         * for a default -- including a hotkey, which is the one setting whose
         * loss is invisible: nothing logs, no key works, and the daemon looks
         * healthy throughout. Keeping the last good config means a typo costs a
         * warning rather than a dictation session. */
        if (reloading) {
            config next;
            if (config_load(&next, cfg_path) < 0)
                log_warn("reloaded config has errors; keeping the previous "
                         "settings\n");
            else
                cfg = next;
        }
    } while (reloading);

    return rc;
}
