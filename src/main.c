/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "audio.h"
#include "config.h"
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
        if (!text)
            continue;

        /* Leading space is common from Whisper and reads as a typo. */
        char *start = text;
        while (*start == ' ')
            start++;

        if (*start) {
            log_info("transcript: %s\n", start);
            if (!print_only && injector_send(inj, start) < 0)
                log_err("injection failed\n");
        }
        free(text);
    }
}

static void on_signal(int sig)
{
    (void)sig;
    input_stop();                       /* write() is async-signal-safe */
}

static void usage(void)
{
    fprintf(stderr,
        "usage: whisprd [-c CONFIG] [-p] [-v]\n"
        "  -c FILE  config file (default $XDG_CONFIG_HOME/whisprd/config.ini)\n"
        "  -p       print transcripts to stdout instead of injecting them\n"
        "  -v       verbose logging\n");
}

int main(int argc, char **argv)
{
    const char *cfg_path = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:pvh")) != -1) {
        switch (opt) {
        case 'c': cfg_path = optarg; break;
        case 'p': print_only = true; break;
        case 'v': log_verbose = 1; break;
        default:  usage(); return opt == 'h' ? 0 : 2;
        }
    }

    config cfg;
    if (config_load(&cfg, cfg_path) < 0)
        return 1;

    /* Logged before anything can fail, so a permissions error still tells the
     * user which key they were meant to be holding. */
    char hotkey[128];
    config_hotkey_desc(&cfg, hotkey, sizeof(hotkey));
    log_info("hotkey: %s\n", hotkey);

    int rc = 1;
    pthread_t worker_thread;
    bool worker_started = false;

    jobs = queue_create(QUEUE_CAP);
    if (!jobs)
        goto out;

    if (transcribe_init(&cfg) < 0)
        goto out;

    /* The injector claims /dev/uinput before input_init enumerates devices, so
     * our own virtual keyboard already exists and gets skipped there. */
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

    log_info("ready; hold %s to talk\n", hotkey);
    rc = input_run() < 0 ? 1 : 0;
    log_info("shutting down\n");

out:
    audio_shutdown();
    if (jobs)
        queue_close(jobs);
    if (worker_started)
        pthread_join(worker_thread, NULL);
    input_shutdown();
    injector_destroy(inj);
    transcribe_shutdown();
    queue_destroy(jobs);
    return rc;
}
