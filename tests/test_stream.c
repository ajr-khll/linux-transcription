/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Feeds one WAV to the live engine in the same 20 ms chunks the capture stream
 * delivers, and prints the preview as it grows. Answers three questions that
 * decide whether live mode is usable, none of which need a microphone, a
 * compositor or a network:
 *
 *   - does the streaming model decode at all, and finish with the right words?
 *   - how far behind the audio does it run?
 *   - how often does it revise text it already emitted?
 *
 * That last number is the one that matters. Every revision is backspaces sent
 * to whatever window has focus, so a model that changes its mind constantly
 * would make live mode unusable however good its final answer is.
 *
 *     make test-stream
 *     build/test_stream /usr/local/share/scribe/models/streaming-zipformer-en/test.wav
 *
 * Kept out of `make test`: it needs a model a plain checkout does not have.
 *
 * The WAV must be mono 16-bit at 16 kHz. Unlike the Parakeet test there is no
 * resampling here -- a streaming recognizer is fed at the rate it was built
 * for, which is the rate scribe captures at. */

#include "asr.h"
#include "audio.h"
#include "config.h"
#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHUNK_SAMPLES 320                   /* 20 ms, as audio.c uses */

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* True when `next` only adds to `prev`. Anything else means the words already
 * on screen have to be taken back. */
static bool extends(const char *prev, const char *next)
{
    return strncmp(prev, next, strlen(prev)) == 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE.wav [MODEL_DIR]\n", argv[0]);
        return 2;
    }

    size_t n = 0;
    uint32_t rate = 0;
    int16_t *s = wav_load(argv[1], &n, &rate);
    if (!s)
        return 1;

    if (rate != AUDIO_RATE) {
        fprintf(stderr, "%s: need %d Hz, got %u -- the online recognizer does "
                "not resample\n", argv[1], AUDIO_RATE, rate);
        free(s);
        return 1;
    }

    config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (argc > 2)
        snprintf(cfg.live_model_dir, sizeof(cfg.live_model_dir), "%s", argv[2]);

    double t0 = now_seconds();
    if (asr_stream_init(&cfg) < 0) {
        free(s);
        return 1;
    }
    double load = now_seconds() - t0;

    int fails = 0;

    /* Same reload check as test_parakeet: scribe-menu sends SIGHUP on every
     * settings save, so an init that reloads the graphs costs a stall each
     * time. */
    t0 = now_seconds();
    asr_stream_shutdown();
    if (asr_stream_init(&cfg) < 0) {
        fprintf(stderr, "FAILED: re-init after shutdown\n");
        fails++;
    }
    double reload = now_seconds() - t0;
    printf("%s  reload keeps the model  (load %.2fs, reload %.2fs)\n",
           reload < load / 4 ? "PASS" : "FAIL", load, reload);
    if (reload >= load / 4)
        fails++;

    double audio_s = (double)n / rate;
    printf("\n%s: %.2f s at %u Hz, %zu chunks of %d\n\n",
           argv[1], audio_s, rate, n / CHUNK_SAMPLES, CHUNK_SAMPLES);

    if (asr_stream_begin() < 0) {
        free(s);
        return 1;
    }

    char  *shown = strdup("");
    size_t updates = 0, revisions = 0;
    double decode_t = 0;

    for (size_t off = 0; off < n; off += CHUNK_SAMPLES) {
        size_t take = n - off < CHUNK_SAMPLES ? n - off : CHUNK_SAMPLES;

        double c0 = now_seconds();
        char *text = asr_stream_feed(s + off, take);
        decode_t += now_seconds() - c0;
        if (!text)
            continue;

        updates++;
        if (!extends(shown, text)) {
            revisions++;
            printf("  %6.2fs  [revised] %s\n", (double)(off + take) / rate, text);
        } else {
            printf("  %6.2fs  %s\n", (double)(off + take) / rate, text);
        }
        free(shown);
        shown = text;
    }

    char *final_text = asr_stream_end();
    free(s);

    printf("\n  final:    %s\n", final_text ? final_text : "(none)");
    if (final_text && !extends(shown, final_text))
        printf("  the tail flush revised the preview\n");
    printf("\n%zu update(s), %zu revision(s)\n", updates, revisions);

    /* Real time is the bar. Above it the preview falls further behind the
     * speaker with every chunk and never catches up. */
    printf("%s  decodes faster than real time  (%.2fs of work for %.2fs of "
           "audio, %.0f%%)\n",
           decode_t < audio_s ? "PASS" : "FAIL", decode_t, audio_s,
           decode_t / audio_s * 100);
    if (decode_t >= audio_s)
        fails++;

    if (!final_text || !final_text[0]) {
        printf("FAIL  the stream produced no text\n");
        fails++;
    } else {
        printf("PASS  the stream produced text\n");
    }

    free(shown);
    free(final_text);
    asr_stream_shutdown();

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
