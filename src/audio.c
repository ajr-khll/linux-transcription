/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* The capture stream runs on a pa_threaded_mainloop rather than pa_simple, for
 * one reason: pa_simple cannot pass PA_STREAM_DONT_MOVE. Without that flag the
 * server's stream-restore module is free to reroute us the moment it decides
 * some other source is a better default, and it does so silently -- the
 * configured `source =` is treated as an opening suggestion, not a choice. That
 * is how a working setup ends up recording a laptop's front-panel hiss.
 *
 * The one-shot paths below (audio_measure_peak, audio_enumerate_sources) stay
 * on pa_simple and pa_mainloop. They record for 400 ms and exit; nothing has
 * time to move them, and they are diagnostics either way. */

#include "audio.h"
#include "cue.h"
#include "log.h"
#include "vad.h"

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

static pa_threaded_mainloop *mainloop;
static pa_context           *ctx;
static pa_stream            *rec;

static queue      *out_queue;
static atomic_bool capturing;

/* The source we asked for, kept for the mismatch check and for the error we
 * print when the stream dies under us. Empty means "the server's default". */
static char        want_source[256];

/* Both are touched only from the mainloop thread, or from audio_init and
 * audio_shutdown while they hold the mainloop lock. */
static bool        stream_up;
static bool        tearing_down;

/* Rolling window of the most recent audio. Seeded into an utterance on the
 * rising edge so the leading consonant is never lost to the gap between the
 * physical keypress and us observing it. */
static int16_t preroll[PREROLL_N];
static size_t  preroll_pos;
static bool    preroll_full;

static int16_t *utt;
static size_t   utt_n, utt_cap;
static bool     was_capturing;

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

    /* Whisper answers a noise floor with hallucinated caption boilerplate
     * ("thanks for watching", stray copyright lines) rather than an empty
     * string, so a mic pointed at nothing looks like a working one returning
     * nonsense. Catch it here: there is no speech to find, and the request
     * would only cost money to get garbage back. */
    vad_report r;
    if (!vad_has_speech(utt, utt_n, &r)) {
        /* Both numbers, every time, so a wrongly rejected utterance can be
         * diagnosed from the journal without reproducing it. */
        log_warn("%.2f s rejected as %s: peak %.1f%% of full scale, frame level "
                 "p10 %.1f dBFS, p90 %.1f dBFS, spread %.1f dB (need %.1f)\n",
                 (double)utt_n / AUDIO_RATE, vad_verdict_str(r.verdict),
                 r.peak / 327.68, r.p10_db, r.p90_db, r.spread_db,
                 VAD_MIN_SPREAD_DB);
        if (r.verdict == VAD_TOO_QUIET)
            log_warn("that source is silent; if you were speaking, set "
                     "'source =' in the config (see: scribe --list-sources)\n");
        else
            log_warn("that source carries a steady noise floor and no voice; "
                     "scribe is probably on the wrong microphone "
                     "(see: scribe --list-sources)\n");
        cue_play(CUE_ERROR);
        utt_n = 0;
        return;
    }
    log_dbg("utterance peak %.1f%% of full scale, envelope spread %.1f dB\n",
            r.peak / 327.68, r.spread_db);

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

/* The hotkey edge, evaluated against a run of samples. Unchanged in substance
 * from the old read loop; it just runs on the mainloop thread now. */
static void feed_chunk(const int16_t *s, size_t n)
{
    bool now = atomic_load(&capturing);
    if (now && !was_capturing)
        utt_begin();
    if (now)
        utt_append(s, n);
    else if (was_capturing)
        utt_seal();

    preroll_write(s, n);
    was_capturing = now;
}

/* Splits one peeked fragment into aligned chunks. pa_stream_peek hands back a
 * pointer into the server's shared memory, which carries no alignment promise
 * for int16_t, so the samples are copied out before anything reads them. */
static void feed(const void *data, size_t nbytes)
{
    const char *p = data;
    int16_t chunk[CHUNK_SAMPLES];

    while (nbytes >= sizeof(int16_t)) {
        size_t take = nbytes / sizeof(int16_t);
        if (take > CHUNK_SAMPLES)
            take = CHUNK_SAMPLES;
        memcpy(chunk, p, take * sizeof(int16_t));
        feed_chunk(chunk, take);
        p += take * sizeof(int16_t);
        nbytes -= take * sizeof(int16_t);
    }
}

static void on_readable(pa_stream *s, size_t nbytes, void *user)
{
    (void)nbytes;
    (void)user;

    while (pa_stream_readable_size(s) > 0) {
        const void *data;
        size_t len;

        if (pa_stream_peek(s, &data, &len) < 0) {
            log_err("cannot read the capture stream: %s\n",
                    pa_strerror(pa_context_errno(ctx)));
            return;
        }
        if (len == 0)
            break;                      /* nothing buffered after all */
        if (data)
            feed(data, len);
        else
            log_dbg("capture overrun: %zu bytes dropped by the server\n", len);
        pa_stream_drop(s);              /* a hole still has to be dropped */
    }
}

static void on_context_state(pa_context *c, void *user)
{
    (void)c;
    (void)user;
    pa_threaded_mainloop_signal(mainloop, 0);
}

static void on_stream_state(pa_stream *s, void *user)
{
    (void)user;

    switch (pa_stream_get_state(s)) {
    case PA_STREAM_READY:
        stream_up = true;
        break;
    case PA_STREAM_FAILED:
    case PA_STREAM_TERMINATED:
        /* The tradeoff DONT_MOVE buys: unplugging the configured mic kills the
         * stream instead of silently migrating it to whatever the server likes.
         * Silence with an explanation beats transcribing the wrong room, but it
         * has to be said out loud -- nothing else will notice. */
        if (stream_up && !tearing_down) {
            log_err("capture stream died -- source '%s' has gone away\n",
                    want_source[0] ? want_source : "(default)");
            log_err("reconnect it, then: systemctl --user reload scribe "
                    "(or send SIGHUP)\n");
            cue_play(CUE_ERROR);
        }
        stream_up = false;
        break;
    default:
        break;
    }
    pa_threaded_mainloop_signal(mainloop, 0);
}

int audio_init(const config *cfg, queue *out)
{
    out_queue = out;
    snprintf(want_source, sizeof(want_source), "%s", cfg->source);
    stream_up = false;
    tearing_down = false;
    was_capturing = false;

    /* No resampling anywhere: ask the server for exactly what we ship. */
    const pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = AUDIO_RATE,
        .channels = 1,
    };
    /* Small fragments keep the read callback arriving every 20 ms or so, which
     * is what bounds how late a hotkey edge can be applied. */
    const pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .fragsize = CHUNK_SAMPLES * sizeof(int16_t),
        .tlength = (uint32_t)-1,
        .prebuf = (uint32_t)-1,
        .minreq = (uint32_t)-1,
    };

    mainloop = pa_threaded_mainloop_new();
    if (!mainloop) {
        log_err("cannot create the audio mainloop\n");
        return -1;
    }
    if (pa_threaded_mainloop_start(mainloop) < 0) {
        log_err("cannot start the audio mainloop\n");
        pa_threaded_mainloop_free(mainloop);
        mainloop = NULL;
        return -1;
    }

    pa_threaded_mainloop_lock(mainloop);

    ctx = pa_context_new(pa_threaded_mainloop_get_api(mainloop), "scribe");
    if (!ctx) {
        log_err("cannot create an audio server connection\n");
        goto fail;
    }
    pa_context_set_state_callback(ctx, on_context_state, NULL);
    if (pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        log_err("cannot connect to the audio server: %s\n",
                pa_strerror(pa_context_errno(ctx)));
        goto fail;
    }
    for (;;) {
        pa_context_state_t st = pa_context_get_state(ctx);
        if (st == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(st)) {
            log_err("audio server connection failed: %s\n",
                    pa_strerror(pa_context_errno(ctx)));
            goto fail;
        }
        pa_threaded_mainloop_wait(mainloop);
    }

    rec = pa_stream_new(ctx, "voice transcription", &ss, NULL);
    if (!rec) {
        log_err("cannot create the capture stream: %s\n",
                pa_strerror(pa_context_errno(ctx)));
        goto fail;
    }
    pa_stream_set_state_callback(rec, on_stream_state, NULL);
    pa_stream_set_read_callback(rec, on_readable, NULL);

    /* DONT_MOVE is the point of all this: it tells the server the stream is
     * pinned and stream-restore must not reroute it. ADJUST_LATENCY makes the
     * server honour the fragsize above instead of picking its own. */
    const char *dev = want_source[0] ? want_source : NULL;
    if (pa_stream_connect_record(rec, dev, &attr,
                                 PA_STREAM_ADJUST_LATENCY |
                                 PA_STREAM_DONT_MOVE) < 0) {
        log_err("cannot open capture stream%s%s: %s\n",
                dev ? " on source " : "", dev ? dev : "",
                pa_strerror(pa_context_errno(ctx)));
        log_err("list available sources with: pactl list short sources\n");
        goto fail;
    }
    for (;;) {
        pa_stream_state_t st = pa_stream_get_state(rec);
        if (st == PA_STREAM_READY)
            break;
        if (st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED) {
            log_err("capture stream refused%s%s: %s\n",
                    dev ? " on source " : "", dev ? dev : "",
                    pa_strerror(pa_context_errno(ctx)));
            log_err("list available sources with: pactl list short sources\n");
            goto fail;
        }
        pa_threaded_mainloop_wait(mainloop);
    }

    /* Which device we actually got, not which one we asked for. "capture
     * stream open" on its own is the log line that let this bug hide: it is
     * equally true whichever microphone the server handed over. */
    const char *got = pa_stream_get_device_name(rec);
    if (!got)
        got = "(unknown)";
    if (want_source[0] && strcmp(got, want_source) != 0) {
        log_err("asked for source '%s' but the server gave us '%s'\n",
                want_source, got);
        log_err("check the name against: pactl list short sources\n");
    }
    log_info("capture stream open on %s (%d Hz mono S16)\n", got, AUDIO_RATE);

    pa_threaded_mainloop_unlock(mainloop);
    return 0;

fail:
    pa_threaded_mainloop_unlock(mainloop);
    audio_shutdown();
    return -1;
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
    pa_context *c = pa_context_new(pa_mainloop_get_api(ml), "scribe");
    if (!c || pa_context_connect(c, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        log_err("cannot connect to the audio server\n");
        if (c)
            pa_context_unref(c);
        pa_mainloop_free(ml);
        return NULL;
    }

    for (;;) {
        pa_context_state_t st = pa_context_get_state(c);
        if (st == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(st)) {
            log_err("audio server connection failed\n");
            pa_context_unref(c);
            pa_mainloop_free(ml);
            return NULL;
        }
        pa_mainloop_iterate(ml, 1, NULL);
    }

    src_list list = { 0 };
    pa_operation *op = pa_context_get_source_info_list(c, on_source, &list);
    while (op && pa_operation_get_state(op) == PA_OPERATION_RUNNING)
        pa_mainloop_iterate(ml, 1, NULL);
    if (op)
        pa_operation_unref(op);

    pa_context_disconnect(c);
    pa_context_unref(c);
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
           VAD_SILENCE_PEAK / 327.68);
    printf("silence, and scribe will refuse to transcribe from it. A source that\n");
    printf("reads high while nobody is speaking is hiss, and gets refused too.\n");

    free(v);
    return 0;
}

void audio_shutdown(void)
{
    if (!mainloop)
        return;

    pa_threaded_mainloop_lock(mainloop);
    tearing_down = true;            /* our own teardown is not a device loss */
    if (rec) {
        pa_stream_set_read_callback(rec, NULL, NULL);
        pa_stream_set_state_callback(rec, NULL, NULL);
        pa_stream_disconnect(rec);
        pa_stream_unref(rec);
        rec = NULL;
    }
    if (ctx) {
        pa_context_set_state_callback(ctx, NULL, NULL);
        pa_context_disconnect(ctx);
        pa_context_unref(ctx);
        ctx = NULL;
    }
    pa_threaded_mainloop_unlock(mainloop);

    pa_threaded_mainloop_stop(mainloop);
    pa_threaded_mainloop_free(mainloop);
    mainloop = NULL;

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
    stream_up = false;
    was_capturing = false;
    atomic_store(&capturing, false);
}
