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
 * transcript and waits. This thread owns the decoding stream, the injector and
 * the record of what is on screen, and does every keystroke itself -- which is
 * the whole reason the other two do not touch the injector at all.
 *
 * What is on screen is tracked two ways, because one of them is not enough.
 * `shown` is the text we believe we typed, and drives the diff that keeps the
 * retraction short. `typed` is how many codepoints actually landed, counted
 * from what the backend reports. They disagree when a backend drops characters
 * it cannot produce -- the uinput layout does this for curly quotes and em
 * dashes -- and when they disagree the count is the one to trust, because it is
 * measured rather than assumed. Erasing by the wrong number does not leave a
 * mess on screen; it eats the user's own text to the left of the caret.
 */
#include "live.h"
#include "asr.h"
#include "audio.h"
#include "log.h"

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

static injector *inj;
static bool      running;                   /* live_init said yes */

static pthread_t       thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wake = PTHREAD_COND_INITIALIZER;   /* work to do */
static pthread_cond_t  done = PTHREAD_COND_INITIALIZER;   /* commit applied */

static enum state state;
static bool  quitting;
static bool  begin_pending;

/* One slot, and that is safe: only the worker thread commits, and it waits for
 * the swap before returning, so a second can never arrive while one is here. */
static char *commit_text;
static bool  commit_pending;

static int16_t *ring;
static size_t   ring_head, ring_tail, ring_count;
static bool     overran;

/* Touched only by the preview thread. */
static char  *shown;                        /* what we think we typed */
static size_t typed;                        /* codepoints that really landed */
static bool   synced;                       /* do the two above agree? */

/* ---- what is on screen ------------------------------------------------- */

/* Bytes the two share, backed off to a codepoint boundary so a retraction can
 * never cut a multi-byte character in half. */
static size_t common_prefix(const char *a, const char *b)
{
    size_t i = 0;
    while (a[i] && a[i] == b[i])
        i++;
    /* Walk back off any continuation bytes: the first differing byte may be the
     * middle of a character whose earlier bytes matched. */
    while (i > 0 && ((unsigned char)a[i] & 0xC0) == 0x80)
        i--;
    return i;
}

/* Erases everything we put on screen and forgets what it was. Used when the
 * two records disagree: the count is still right, the text is not, so the only
 * honest move is to clear the ground and start again. */
static void resync(void)
{
    if (typed > 0 && injector_erase(inj, typed) < 0)
        log_warn("live: could not clear the preview; leaving it alone\n");
    typed = 0;
    free(shown);
    shown = strdup("");
    synced = true;
}

/* Moves the screen from `shown` to `want`, erasing only the tail they do not
 * share. Returns with `shown` equal to `want` when it worked. */
static void render(const char *want)
{
    if (!synced)
        resync();

    size_t keep = common_prefix(shown, want);

    size_t drop = injector_utf8_len(shown + keep);
    if (drop > 0) {
        if (injector_erase(inj, drop) < 0) {
            synced = false;
            return;
        }
        typed -= drop;
    }

    const char *tail = want + keep;
    if (*tail) {
        size_t landed = 0;
        int rc = injector_send(inj, tail, &landed);
        typed += landed;
        if (rc < 0 || landed != injector_utf8_len(tail)) {
            /* Either the send failed part-way or the backend could not produce
             * some characters. Both leave us unable to say what is on screen,
             * so mark it and let the next render clear up. */
            synced = false;
            log_dbg("live: typed %zu of %zu, preview out of step\n",
                    landed, injector_utf8_len(tail));
            return;
        }
    }

    char *copy = strdup(want);
    if (copy) {
        free(shown);
        shown = copy;
    } else {
        synced = false;
    }
}

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
    render(text);
    free(text);
}

/* The key is up and the ring is empty: flush the decoder's tail, show it, and
 * leave the preview standing until the real transcript arrives. */
static void finish_stream(void)
{
    char *text = asr_stream_end();
    if (text) {
        render(text);
        free(text);
    }
}

/* Swaps the preview for the finished transcript, then forgets both. Forgetting
 * is the point: once this returns the text belongs to the user's document, and
 * the next utterance must not try to erase it. */
static void apply_commit(const char *final_text)
{
    if (!synced)
        resync();
    render(final_text);

    free(shown);
    shown = strdup("");
    typed = 0;
    synced = true;
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
            pthread_mutex_unlock(&lock);

            apply_commit(text ? text : "");
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
    shown = strdup("");
    if (!ring || !shown) {
        log_err("live: out of memory\n");
        free(ring);
        free(shown);
        ring = NULL;
        shown = NULL;
        return false;
    }

    inj = injector_in;
    state = ST_IDLE;
    quitting = false;
    begin_pending = commit_pending = false;
    typed = 0;
    synced = true;
    ring_reset();

    if (pthread_create(&thread, NULL, live_thread, NULL) != 0) {
        log_err("live: cannot start the preview thread\n");
        free(ring);
        free(shown);
        ring = NULL;
        shown = NULL;
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

void live_commit(const char *final_text)
{
    if (!running)
        return;

    char *copy = strdup(final_text ? final_text : "");
    if (!copy) {
        log_err("live: out of memory committing a transcript\n");
        return;
    }

    pthread_mutex_lock(&lock);
    free(commit_text);
    commit_text = copy;
    commit_pending = true;
    pthread_cond_signal(&wake);

    /* Waiting is what makes this thread the only one that types. The worker has
     * just finished decoding and has nothing else to do, so the cost is the
     * keystrokes themselves. */
    while (commit_pending && !quitting)
        pthread_cond_wait(&done, &lock);
    pthread_mutex_unlock(&lock);
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
     * never asked for and cannot account for. Take it back. */
    if (typed > 0)
        injector_erase(inj, typed);

    free(ring);
    free(shown);
    free(commit_text);
    ring = NULL;
    shown = NULL;
    commit_text = NULL;
    inj = NULL;
    typed = 0;
    running = false;

    /* Empty by design, exactly as the other engines' shutdowns are: a SIGHUP
     * reload comes through here and must not drop the model. */
    asr_stream_shutdown();
}
