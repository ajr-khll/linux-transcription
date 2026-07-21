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
#include "cue.h"
#include "history.h"
#include "injector.h"
#include "input.h"
#include "log.h"
#include "queue.h"
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

/* Runs on the input thread, so it must not block. */
static void on_hold(bool holding, void *user)
{
    (void)user;
    log_dbg("%s\n", holding ? "HOLD_START" : "HOLD_STOP");
    audio_set_capturing(holding);
    cue_play(holding ? CUE_START : CUE_STOP);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pcm_buffer *b = queue_pop(jobs);
        if (!b)
            return NULL;                /* queue closed: shutting down */

        char *text = transcribe_pcm(b->samples, b->n_samples);
        pcm_buffer_free(b);
        if (!text) {
            cue_play(CUE_ERROR);            /* network or endpoint failure */
            continue;
        }

        /* Leading space is common from Whisper and reads as a typo. */
        char *start = text;
        while (*start == ' ')
            start++;

        if (*start) {
            log_info("transcript: %s\n", start);
            /* Recorded before injection so a failing backend still leaves the
             * transcript somewhere the user can get at it. */
            history_append(start);
            if (!print_only && injector_send(inj, start) < 0) {
                log_err("injection failed\n");
                cue_play(CUE_ERROR);
            }
        } else {
            cue_play(CUE_ERROR);            /* nothing was recognised */
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
    int rc = injector_send(i, text) < 0 ? 1 : 0;
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
        }

        if (input_init(&cfg, on_hold, NULL) < 0)
            goto out;

        if (audio_init(&cfg, jobs) < 0)
            goto out;

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

        /* input_run returns for either reason; the flag says which. */
        reloading = reload_requested != 0;
        log_info("%s\n", reloading ? "SIGHUP: reloading configuration"
                                   : "shutting down");

    out:
        audio_shutdown();
        if (jobs)
            queue_close(jobs);
        if (worker_started)
            pthread_join(worker_thread, NULL);
        input_shutdown();
        injector_destroy(inj);
        history_shutdown();
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
