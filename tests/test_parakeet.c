/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Loads the Parakeet model and decodes one WAV. This is the only part of the
 * daemon that can be tested without a microphone, a compositor or a network,
 * and it links against the sherpa binding alone -- so when dictation comes
 * back empty, this says whether the model or the rest of scribe is at fault.
 *
 *     make test-parakeet
 *     build/test_parakeet /usr/local/share/scribe/models/.../test.wav
 *
 * Kept out of `make test`: it needs a 640 MB model that a plain checkout does
 * not have.
 *
 * The WAV must be mono 16-bit. Any sample rate works -- sherpa-onnx resamples
 * -- which matters because the samples shipped with the model are 22 and
 * 24 kHz, not the 16 kHz scribe captures at. */

#include "asr.h"
#include "audio.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Returns malloc'd samples from a 16-bit mono PCM WAV, or NULL. */
static int16_t *wav_load(const char *path, size_t *n_out, uint32_t *rate_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }

    unsigned char hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a WAV file\n", path);
        fclose(f);
        return NULL;
    }

    uint16_t channels = 0, bits = 0;
    unsigned char ck[8];
    while (fread(ck, 1, sizeof(ck), f) == sizeof(ck)) {
        uint32_t len = rd32(ck + 4);

        if (!memcmp(ck, "fmt ", 4) && len >= 16) {
            unsigned char fmt[16];
            if (fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt))
                break;
            channels  = rd16(fmt + 2);
            *rate_out = rd32(fmt + 4);
            bits      = rd16(fmt + 14);
            fseek(f, (long)len - 16, SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            if (channels != 1 || bits != 16) {
                fprintf(stderr, "%s: need mono 16-bit PCM, got %u ch / %u bit\n",
                        path, channels, bits);
                break;
            }
            size_t n = len / sizeof(int16_t);
            int16_t *s = malloc(n * sizeof(*s));
            if (!s || fread(s, sizeof(*s), n, f) != n) {
                free(s);
                break;
            }
            fclose(f);
            *n_out = n;
            return s;
        } else {
            fseek(f, (long)len + (len & 1), SEEK_CUR);   /* chunks pad to even */
        }
    }

    fprintf(stderr, "%s: no usable data chunk\n", path);
    fclose(f);
    return NULL;
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

    config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (argc > 2)
        snprintf(cfg.model_dir, sizeof(cfg.model_dir), "%s", argv[2]);

    double t0 = now_seconds();
    if (asr_parakeet_init(&cfg) < 0) {
        free(s);
        return 1;
    }
    double load = now_seconds() - t0;

    /* scribe-menu sends SIGHUP on every settings save, and the daemon answers
     * one by calling shutdown and then init again. If that reloads the model,
     * every save costs a multi-second stall -- so a second init with the same
     * settings must keep the recognizer it already has. Cheap to test here,
     * and invisible in normal use until someone times it. */
    int fails = 0;
    t0 = now_seconds();
    if (asr_parakeet_shutdown(), asr_parakeet_init(&cfg) < 0) {
        fprintf(stderr, "FAILED: re-init after shutdown\n");
        fails++;
    }
    double reload = now_seconds() - t0;
    printf("%s  reload keeps the model  (load %.2fs, reload %.2fs)\n",
           reload < load / 4 ? "PASS" : "FAIL", load, reload);
    if (reload >= load / 4)
        fails++;

    printf("%s: %.2f s at %u Hz\n", argv[1], (double)n / rate, rate);

    char *text = asr_parakeet_pcm_at(s, n, (int)rate);
    free(s);
    asr_parakeet_shutdown();

    if (!text) {
        printf("FAIL  decode returned nothing\n");
        fails++;
    } else {
        printf("%s  decoded: %s\n", text[0] ? "PASS" : "FAIL", text);
        if (!text[0])
            fails++;
        free(text);
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
