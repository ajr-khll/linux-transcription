/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* The preview thread.
 *
 * Three threads meet here and only one of them types. The capture thread pushes
 * samples in and never blocks, because it is the PulseAudio mainloop and a
 * stall there costs dropped audio. The worker thread hands over the finished
 * transcript and waits. This thread owns the decoding stream and does every
 * keystroke itself -- which is the whole reason the other two do not touch the
 * injector at all.
 *
 * What is on screen, and the diff that keeps each retraction short, lives in
 * screen.c. This thread is its sole caller while the preview runs; the worker
 * thread is when it does not. The two paths never overlap.
 */
#include "live.h"
#include "asr.h"
#include "audio.h"
#include "log.h"
#include "screen.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Thirty seconds of headroom. The streaming model runs at a few percent of real
 * time, so this only fills if something has gone badly wrong -- and then the
 * preview stops rather than falling further behind for the rest of the hold. */
#define RING_SAMPLES (AUDIO_RATE * 30)

/* How much audio one decode call takes. Matches the capture chunk, so the
 * preview advances at the rate the samples arrive. */
#define DECODE_SAMPLES 320                  /* 20 ms */

enum state {
    ST_IDLE,                                /* between utterances */
    ST_RUNNING,                             /* key down, audio arriving */
    ST_CLOSING,                             /* key up, ring still draining */
};

static injector *inj;                       /* borrowed, for screen_attach */
static bool      running;                   /* live_init said yes */

static pthread_t       thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wake = PTHREAD_COND_INITIALIZER;   /* work to do */
static pthread_cond_t  done = PTHREAD_COND_INITIALIZER;   /* commit applied */

static enum state state;
static bool  quitting;
static bool  begin_pending;

/* One slot, and that is safe: only the worker thread hands text over, and it
 * waits for the swap before returning, so a second can never arrive while one
 * is here. The op is either a stage (put text on screen, keep owning it) or a
 * commit (put it on screen and hand it to the user). */
static char    *commit_text;
static bool     commit_pending;
static bool     commit_release;         /* commit hands the text over; stage does not */
static bool     commit_check_gen;       /* a staged correction: drop it if stale */
static unsigned commit_gen;             /* the generation the correction belongs to */

/* Bumped on every live_begin. A cleanup correction computed for one utterance
 * must not land on screen after the next utterance's preview has started -- it
 * would erase words it never typed. The worker stamps each correction with the
 * generation it staged under; a mismatch here means the user moved on, so the
 * raw transcript already on screen stands and the correction is dropped. */
static unsigned generation;

static int16_t *ring;
static size_t   ring_head, ring_tail, ring_count;
static bool     overran;

/* ---- the ring ---------------------------------------------------------- */

/* Caller holds the lock. */
static void ring_reset(void)
{
    ring_head = ring_tail = ring_count = 0;
    overran = false;
}

/* ---- the thread -------------------------------------------------------- */

/* Feeds one chunk to the model and puts whatever it says on screen. Runs
 * without the lock: decoding takes milliseconds and typing takes longer. */
static void decode_chunk(const int16_t *s, size_t n)
{
    char *text = asr_stream_feed(s, n);
    if (!text)
        return;                             /* no new words, the common case */
    screen_render(text);
    free(text);
}

/* The key is up and the ring is empty: flush the decoder's tail, show it, and
 * leave the preview standing until the real transcript arrives. */
static void finish_stream(void)
{
    char *text = asr_stream_end();
    if (text) {
        screen_render(text);
        free(text);
    }
}

static void *live_thread(void *arg)
{
    (void)arg;
    int16_t chunk[DECODE_SAMPLES];

    for (;;) {
        pthread_mutex_lock(&lock);

        while (!quitting && !commit_pending && !begin_pending && ring_count == 0 &&
               !(state == ST_CLOSING))
            pthread_cond_wait(&wake, &lock);

        if (quitting) {
            pthread_mutex_unlock(&lock);
            break;
        }

        /* Before anything else: a commit belongs to the utterance that just
         * ended, and a begin to the one starting. Taking them in this order is
         * what keeps a fast second keypress from stepping on the swap. */
        if (commit_pending) {
            char *text = commit_text;
            commit_text = NULL;
            bool release = commit_release;
            /* A staged correction is stale when a newer utterance has begun
             * since it was staged. generation is only written under this lock,
             * so read it here while we hold it. */
            bool stale = commit_check_gen && commit_gen != generation;
            pthread_mutex_unlock(&lock);

            if (stale) {
                log_dbg("live: dropping a stale correction; the raw text stands\n");
            } else {
                screen_render(text ? text : "");
                if (release)
                    screen_release();
            }
            free(text);

            pthread_mutex_lock(&lock);
            commit_pending = false;
            pthread_cond_broadcast(&done);
            pthread_mutex_unlock(&lock);
            continue;
        }

        /* Consumed whatever the state has since become. A tap short enough to
         * release before this thread woke up leaves the state already CLOSING,
         * and the stream still has to be opened so the branch below has
         * something to close -- and so this flag cannot survive to spin the
         * loop forever. */
        if (begin_pending) {
            begin_pending = false;
            pthread_mutex_unlock(&lock);

            /* Anything a prior utterance staged and never committed is the
             * user's now: forget it, so this utterance's preview types fresh
             * text rather than diffing against the last one's transcript. In
             * the common case nothing is owned and this does nothing. */
            screen_release();

            if (asr_stream_begin() < 0)
                log_warn("live: no preview for this utterance\n");
            continue;
        }

        if (ring_count > 0) {
            size_t take = ring_count < DECODE_SAMPLES ? ring_count : DECODE_SAMPLES;
            for (size_t i = 0; i < take; i++) {
                chunk[i] = ring[ring_tail];
                ring_tail = (ring_tail + 1) % RING_SAMPLES;
            }
            ring_count -= take;
            pthread_mutex_unlock(&lock);

            decode_chunk(chunk, take);
            continue;
        }

        if (state == ST_CLOSING) {
            pthread_mutex_unlock(&lock);
            finish_stream();
            pthread_mutex_lock(&lock);
            state = ST_IDLE;
            pthread_mutex_unlock(&lock);
            continue;
        }

        pthread_mutex_unlock(&lock);
    }

    return NULL;
}

/* ---- the interface ----------------------------------------------------- */

bool live_init(const config *cfg, injector *injector_in)
{
    if (running)
        return true;

    if (!cfg->live) {
        log_dbg("live: off in the config\n");
        return false;
    }
    if (!injector_in) {
        log_dbg("live: nothing to type into\n");
        return false;
    }

    /* The preview only replaces text it typed itself, and the thing that
     * replaces it is the local engine's answer. Against the network engine
     * there is nothing to gain and a round trip to wait through. */
    if (strcmp(cfg->engine, "parakeet") != 0) {
        log_info("live preview off: it needs engine = parakeet\n");
        return false;
    }
    /* The clipboard backend pastes rather than types, so nothing it puts on
     * screen can be taken back. */
    if (!injector_can_erase(injector_in)) {
        log_info("live preview off: this injection backend cannot erase\n");
        return false;
    }
    /* The preview types while the key is still down. If that key is a modifier,
     * every character becomes a shortcut in whatever window has focus -- Ctrl+V
     * pastes, Ctrl+W closes it, Ctrl+Q quits it. Zeroing the virtual keyboard's
     * own modifiers does not help: the user's physical Ctrl really is down, and
     * the compositor is right to say so.
     *
     * Batch injection never met this, because it types after the release. There
     * is no fix on this side, so refuse and name the one the user has. */
    if (config_hotkey_holds_modifier(cfg)) {
        char hotkey[128];
        config_hotkey_desc(cfg, hotkey, sizeof(hotkey));
        log_warn("live preview off: holding %s keeps a modifier down, and the "
                 "preview would type shortcuts into your window rather than "
                 "text\n", hotkey);
        log_warn("to use it, pick a hotkey that is not a modifier -- a spare "
                 "key such as KEY_F13 or KEY_SCROLLLOCK, or one you do not "
                 "otherwise type\n");
        return false;
    }
    if (asr_stream_init(cfg) < 0) {
        log_warn("live preview off: the streaming model would not load\n");
        return false;
    }

    ring = malloc(RING_SAMPLES * sizeof(*ring));
    if (!ring) {
        log_err("live: out of memory\n");
        return false;
    }

    inj = injector_in;
    screen_attach(inj);
    state = ST_IDLE;
    quitting = false;
    begin_pending = commit_pending = false;
    generation = 0;
    ring_reset();

    if (pthread_create(&thread, NULL, live_thread, NULL) != 0) {
        log_err("live: cannot start the preview thread\n");
        screen_detach();
        free(ring);
        ring = NULL;
        inj = NULL;
        return false;
    }

    running = true;
    log_info("live preview on: words appear as you speak, and are replaced by "
             "the transcript when you let go\n");
    return true;
}

bool live_active(void)
{
    return running;
}

void live_begin(void)
{
    if (!running)
        return;
    pthread_mutex_lock(&lock);
    /* Cleared here rather than on the preview thread so that samples handed
     * over between now and the stream actually opening are kept. The capture
     * thread feeds the preroll immediately after this call, and that is the
     * opening consonant -- the one thing the preview most needs. */
    ring_reset();
    state = ST_RUNNING;
    begin_pending = true;
    /* A new utterance: any correction the worker is still computing for the
     * last one is now stale. See the note on `generation`. */
    generation++;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

void live_feed(const int16_t *samples, size_t n_samples)
{
    if (!running)
        return;

    pthread_mutex_lock(&lock);
    if (state != ST_RUNNING) {
        /* The key is up. These samples belong to no utterance. */
        pthread_mutex_unlock(&lock);
        return;
    }
    if (ring_count + n_samples > RING_SAMPLES) {
        if (!overran) {
            overran = true;
            log_warn("live: the preview fell too far behind; it stops here. "
                     "The transcript is unaffected.\n");
        }
        pthread_mutex_unlock(&lock);
        return;
    }
    for (size_t i = 0; i < n_samples; i++) {
        ring[ring_head] = samples[i];
        ring_head = (ring_head + 1) % RING_SAMPLES;
    }
    ring_count += n_samples;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

void live_close(void)
{
    if (!running)
        return;
    pthread_mutex_lock(&lock);
    if (state == ST_RUNNING)
        state = ST_CLOSING;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

/* Hands `text` to the preview thread and waits until it is on screen. `release`
 * decides whether the text becomes the user's afterwards; `check_gen`, whether a
 * newer utterance since `gen` should cause it to be dropped. Returns the
 * generation the handoff was stamped with. Waiting is what makes the preview
 * thread the only one that types: the worker has nothing else to do meanwhile,
 * so the cost is the keystrokes themselves. */
static unsigned handoff(const char *text, bool release, bool check_gen, unsigned gen)
{
    char *copy = strdup(text ? text : "");
    if (!copy) {
        log_err("live: out of memory handing over a transcript\n");
        return gen;
    }

    pthread_mutex_lock(&lock);
    unsigned now = generation;
    free(commit_text);
    commit_text = copy;
    commit_release = release;
    commit_check_gen = check_gen;
    commit_gen = gen;
    commit_pending = true;
    pthread_cond_signal(&wake);

    while (commit_pending && !quitting)
        pthread_cond_wait(&done, &lock);
    pthread_mutex_unlock(&lock);
    return now;
}

/* Puts `text` on screen and keeps owning it, so a later stage or commit can
 * replace just the tail that changed. Returns the generation to stamp the
 * matching commit with. */
unsigned live_stage(const char *text)
{
    if (!running)
        return 0;
    return handoff(text, false, false, 0);
}

/* Replaces the staged text with the finished one and hands it to the user --
 * unless a newer utterance has begun since `gen`, in which case the raw text
 * already on screen stands and this is dropped. */
void live_commit_staged(const char *final_text, unsigned gen)
{
    if (!running)
        return;
    handoff(final_text, true, true, gen);
}

/* Puts `final_text` on screen and hands it to the user, unconditionally. The
 * path with no cleanup pass, and the retraction of a preview that produced
 * nothing. */
void live_commit(const char *final_text)
{
    if (!running)
        return;
    handoff(final_text, true, false, 0);
}

void live_shutdown(void)
{
    if (!running)
        return;

    pthread_mutex_lock(&lock);
    quitting = true;
    pthread_cond_broadcast(&wake);
    pthread_cond_broadcast(&done);
    pthread_mutex_unlock(&lock);

    pthread_join(thread, NULL);

    /* A preview still on screen when the daemon goes down is text the user
     * never asked for and cannot account for. screen_detach takes it back. */
    screen_detach();

    free(ring);
    free(commit_text);
    ring = NULL;
    commit_text = NULL;
    inj = NULL;
    running = false;

    /* Empty by design, exactly as the other engines' shutdowns are: a SIGHUP
     * reload comes through here and must not drop the model. */
    asr_stream_shutdown();
}
