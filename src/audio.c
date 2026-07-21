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
#include "cue.h"
#include "log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#define CHUNK_SAMPLES  320                      /* 20 ms */
#define PREROLL_MS     250
#define PREROLL_N      (AUDIO_RATE * PREROLL_MS / 1000)
#define MAX_UTTERANCE  (AUDIO_RATE * 120)       /* hard cap: 2 minutes */

/* ~2% of full scale. A live mic in a quiet room sits well above this; an
 * unplugged input's noise floor sits well below it. */
#define SILENCE_PEAK   655

static pa_simple      *stream;
static queue          *out_queue;
static pthread_t       thread;
static atomic_bool     capturing;
static atomic_bool     running;

/* Rolling window of the most recent audio. Seeded into an utterance on the
 * rising edge so the leading consonant is never lost to the gap between the
 * physical keypress and us observing it. */
static int16_t preroll[PREROLL_N];
static size_t  preroll_pos;
static bool    preroll_full;

static int16_t *utt;
static size_t   utt_n, utt_cap;

static void preroll_write(const int16_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        preroll[preroll_pos] = src[i];
        preroll_pos = (preroll_pos + 1) % PREROLL_N;
        if (preroll_pos == 0)
            preroll_full = true;
    }
}

static int utt_append(const int16_t *src, size_t n)
{
    if (utt_n + n > MAX_UTTERANCE)
        n = utt_n < MAX_UTTERANCE ? MAX_UTTERANCE - utt_n : 0;
    if (n == 0)
        return 0;

    if (utt_n + n > utt_cap) {
        size_t cap = utt_cap ? utt_cap * 2 : AUDIO_RATE;   /* start at 1 s */
        while (cap < utt_n + n)
            cap *= 2;
        int16_t *p = realloc(utt, cap * sizeof(*p));
        if (!p) {
            log_err("out of memory growing utterance buffer\n");
            return -1;
        }
        utt = p;
        utt_cap = cap;
    }
    memcpy(utt + utt_n, src, n * sizeof(*src));
    utt_n += n;
    return 0;
}

static void utt_begin(void)
{
    utt_n = 0;
    size_t n = preroll_full ? PREROLL_N : preroll_pos;
    if (n == 0)
        return;
    /* Oldest sample first. */
    size_t start = preroll_full ? preroll_pos : 0;
    for (size_t i = 0; i < n; i++) {
        int16_t s = preroll[(start + i) % PREROLL_N];
        utt_append(&s, 1);
    }
}

static void utt_seal(void)
{
    if (utt_n < AUDIO_RATE / 10) {          /* < 100 ms: a stray tap */
        log_dbg("utterance too short (%zu samples), discarded\n", utt_n);
        utt_n = 0;
        return;
    }

    /* Whisper answers silence with hallucinated caption boilerplate ("thanks
     * for watching", stray copyright lines) rather than an empty string, so a
     * dead capture source looks like a working one returning nonsense. Catch
     * it here instead: below this level there is no speech to find, and the
     * request would only cost money to get garbage back. */
    int peak = 0;
    for (size_t i = 0; i < utt_n; i++) {
        int v = utt[i] < 0 ? -utt[i] : utt[i];
        if (v > peak)
            peak = v;
    }
    if (peak < SILENCE_PEAK) {
        log_warn("utterance peaked at %.1f%% of full scale: treating as silence, "
                 "not transcribing\n", peak / 327.68);
        log_warn("if you were speaking, scribe is on the wrong capture source; "
                 "set 'source =' in the config (see: pactl list short sources)\n");
        cue_play(CUE_ERROR);
        utt_n = 0;
        return;
    }
    log_dbg("utterance peak %.1f%% of full scale\n", peak / 327.68);

    pcm_buffer *b = malloc(sizeof(*b));
    if (!b)
        return;
    b->samples = malloc(utt_n * sizeof(int16_t));
    if (!b->samples) {
        free(b);
        return;
    }
    memcpy(b->samples, utt, utt_n * sizeof(int16_t));
    b->n_samples = utt_n;
    utt_n = 0;

    log_dbg("sealed utterance: %.2f s\n", (double)b->n_samples / AUDIO_RATE);
    queue_push(out_queue, b);
}

static void *capture_loop(void *arg)
{
    (void)arg;
    int16_t chunk[CHUNK_SAMPLES];
    bool was_capturing = false;

    while (atomic_load(&running)) {
        int err;
        if (pa_simple_read(stream, chunk, sizeof(chunk), &err) < 0) {
            log_err("pa_simple_read: %s\n", pa_strerror(err));
            break;
        }

        bool now = atomic_load(&capturing);
        if (now && !was_capturing)
            utt_begin();
        if (now)
            utt_append(chunk, CHUNK_SAMPLES);
        else if (was_capturing)
            utt_seal();

        preroll_write(chunk, CHUNK_SAMPLES);
        was_capturing = now;
    }
    return NULL;
}

int audio_init(const config *cfg, queue *out)
{
    (void)cfg;
    out_queue = out;

    /* No resampling anywhere: ask the server for exactly what we ship. */
    const pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = AUDIO_RATE,
        .channels = 1,
    };
    /* Small fragments keep pa_simple_read from blocking long, so shutdown is
     * responsive without any extra wakeup plumbing. */
    const pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .fragsize = CHUNK_SAMPLES * sizeof(int16_t),
        .tlength = (uint32_t)-1,
        .prebuf = (uint32_t)-1,
        .minreq = (uint32_t)-1,
    };

    int err;
    const char *dev = cfg->source[0] ? cfg->source : NULL;
    stream = pa_simple_new(NULL, "scribe", PA_STREAM_RECORD, dev,
                           "voice transcription", &ss, NULL, &attr, &err);
    if (!stream) {
        log_err("cannot open capture stream%s%s: %s\n",
                dev ? " on source " : "", dev ? dev : "", pa_strerror(err));
        log_err("list available sources with: pactl list short sources\n");
        return -1;
    }

    atomic_store(&running, true);
    if (pthread_create(&thread, NULL, capture_loop, NULL) != 0) {
        log_err("cannot start capture thread\n");
        pa_simple_free(stream);
        stream = NULL;
        return -1;
    }
    log_info("capture stream open (%d Hz mono S16)\n", AUDIO_RATE);
    return 0;
}

void audio_set_capturing(bool on)
{
    atomic_store(&capturing, on);
}

/* ---- source enumeration ------------------------------------------------ */

typedef struct {
    audio_source *v;
    size_t        n, cap;
} src_list;

static void on_source(pa_context *c, const pa_source_info *i, int eol, void *user)
{
    (void)c;
    src_list *l = user;
    if (eol || !i)
        return;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        audio_source *p = realloc(l->v, cap * sizeof(*p));
        if (!p)
            return;
        l->v = p;
        l->cap = cap;
    }
    audio_source *e = &l->v[l->n++];
    snprintf(e->name, sizeof(e->name), "%s", i->name ? i->name : "");
    snprintf(e->desc, sizeof(e->desc), "%s", i->description ? i->description : "");
    e->monitor = i->monitor_of_sink != PA_INVALID_INDEX;
}

int audio_measure_peak(const char *source, int ms)
{
    const pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE, .rate = AUDIO_RATE, .channels = 1,
    };
    int err;
    pa_simple *s = pa_simple_new(NULL, "scribe", PA_STREAM_RECORD, source,
                                 "level check", &ss, NULL, NULL, &err);
    if (!s)
        return -1;

    size_t total = (size_t)AUDIO_RATE * (size_t)ms / 1000;
    int16_t chunk[CHUNK_SAMPLES];
    int peak = 0;
    for (size_t done = 0; done < total; done += CHUNK_SAMPLES) {
        if (pa_simple_read(s, chunk, sizeof(chunk), &err) < 0)
            break;
        for (size_t i = 0; i < CHUNK_SAMPLES; i++) {
            int v = chunk[i] < 0 ? -chunk[i] : chunk[i];
            if (v > peak)
                peak = v;
        }
    }
    pa_simple_free(s);
    return peak;
}

audio_source *audio_enumerate_sources(size_t *n_out)
{
    *n_out = 0;

    pa_mainloop *ml = pa_mainloop_new();
    if (!ml)
        return NULL;
    pa_context *ctx = pa_context_new(pa_mainloop_get_api(ml), "scribe");
    if (!ctx || pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        log_err("cannot connect to the audio server\n");
        if (ctx)
            pa_context_unref(ctx);
        pa_mainloop_free(ml);
        return NULL;
    }

    for (;;) {
        pa_context_state_t st = pa_context_get_state(ctx);
        if (st == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(st)) {
            log_err("audio server connection failed\n");
            pa_context_unref(ctx);
            pa_mainloop_free(ml);
            return NULL;
        }
        pa_mainloop_iterate(ml, 1, NULL);
    }

    src_list list = { 0 };
    pa_operation *op = pa_context_get_source_info_list(ctx, on_source, &list);
    while (op && pa_operation_get_state(op) == PA_OPERATION_RUNNING)
        pa_mainloop_iterate(ml, 1, NULL);
    if (op)
        pa_operation_unref(op);

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    *n_out = list.n;
    return list.v;
}

int audio_list_sources(void)
{
    size_t n = 0;
    audio_source *v = audio_enumerate_sources(&n);
    if (!v)
        return -1;

    printf("Sampling each source for 400 ms -- speak now to see your mic respond.\n\n");
    printf("%-6s %-9s %s\n", "PEAK", "KIND", "SOURCE");

    for (size_t i = 0; i < n; i++) {
        int peak = audio_measure_peak(v[i].name, 400);
        char lvl[16];
        if (peak < 0)
            snprintf(lvl, sizeof(lvl), "  n/a");
        else
            snprintf(lvl, sizeof(lvl), "%4.1f%%", peak / 327.68);

        printf("%-6s %-9s %s\n", lvl, v[i].monitor ? "monitor" : "input", v[i].name);
        printf("       %-9s %s\n", "", v[i].desc);
    }

    printf("\nPut the name of the source you want in config as:  source = <name>\n");
    printf("A live microphone reads well above %.1f%%; anything at or below that is\n",
           SILENCE_PEAK / 327.68);
    printf("silence, and scribe will refuse to transcribe from it.\n");

    free(v);
    return 0;
}

void audio_shutdown(void)
{
    if (!stream)
        return;
    atomic_store(&running, false);
    pthread_join(thread, NULL);
    pa_simple_free(stream);
    stream = NULL;
    free(utt);
    utt = NULL;
    utt_cap = utt_n = 0;

    /* SIGHUP tears this down and stands it back up in the same process, so
     * every static here outlives the stream it belongs to. Left set, the
     * preroll would seed the first utterance after a reload with up to 250 ms
     * of audio from the *previous* microphone, and a hotkey still held across
     * the reload would start the new stream mid-utterance. */
    preroll_pos = 0;
    preroll_full = false;
    atomic_store(&capturing, false);
}
