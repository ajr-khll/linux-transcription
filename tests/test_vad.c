/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Run with no arguments for the unit tests.
 *
 * Run with a 16 kHz mono 16-bit WAV to print what the detector measures on it:
 *
 *     parecord --device=<source> --rate=16000 --channels=1 \
 *              --format=s16le --file-format=wav hiss.wav
 *     build/test_vad hiss.wav
 *
 * That is how VAD_MIN_SPREAD_DB gets picked: record the noise floor that fooled
 * the peak test, record yourself talking over the same source, and put the
 * threshold in the gap between the two spreads. */

#include "vad.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE  16000

static int fails;

static void check(const char *what, const int16_t *s, size_t n, bool want)
{
    vad_report r;
    bool got = vad_has_speech(s, n, &r);
    int ok = got == want;
    printf("%s  %-38s -> %-32s peak %5.1f%%  p10 %6.1f  p90 %6.1f  spread %5.1f dB\n",
           ok ? "PASS" : "FAIL", what, vad_verdict_str(r.verdict),
           r.peak / 327.68, r.p10_db, r.p90_db, r.spread_db);
    if (!ok) {
        printf("      wanted: %s\n", want ? "speech" : "reject");
        fails++;
    }
}

/* xorshift, so the fixtures are the same on every machine and a failure is
 * always reproducible. */
static uint32_t rng_state = 0x9e3779b9u;

static void rng_reset(void) { rng_state = 0x9e3779b9u; }

static double rng_bipolar(void)      /* -1.0 .. 1.0 */
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (double)(int32_t)rng_state / 2147483648.0;
}

static void fill_noise(int16_t *s, size_t n, double amp)
{
    for (size_t i = 0; i < n; i++)
        s[i] = (int16_t)(rng_bipolar() * amp);
}

/* Noise whose level drifts, the way an unconnected analog input picks up
 * interference from everything around it. This is the fixture that matters:
 * flat white noise is trivial to reject, and a real dead jack is not flat --
 * the one that prompted all this drifts over 10 dB and clears both the 2% floor
 * and any threshold set from white noise alone. `drift_db` is the peak-to-peak
 * swing of the level. */
static void fill_wandering(int16_t *s, size_t n, double amp, double drift_db)
{
    for (size_t i = 0; i < n; i++) {
        double t = (double)i / RATE;
        /* Two slow components at unrelated periods, so the drift never lines
         * up with the frame boundaries and fakes a syllable rate. */
        double m = 0.5 * sin(2.0 * M_PI * t / 1.7) +
                   0.5 * sin(2.0 * M_PI * t / 0.61);
        double gain = pow(10.0, m * drift_db * 0.5 / 20.0);
        s[i] = (int16_t)(rng_bipolar() * amp * gain);
    }
}

/* Alternating loud and quiet stretches: the envelope speech has and a noise
 * floor does not. `voiced_ms` on, `pause_ms` off, repeated. */
static void fill_syllabic(int16_t *s, size_t n, int voiced_ms, int pause_ms,
                          double amp)
{
    size_t voiced = (size_t)RATE * voiced_ms / 1000;
    size_t pause  = (size_t)RATE * pause_ms / 1000;
    size_t period = voiced + pause;

    for (size_t i = 0; i < n; i++) {
        bool on = (i % period) < voiced;
        /* A 180 Hz carrier under a raised-cosine syllable envelope, plus the
         * room tone that never goes away. */
        double v = rng_bipolar() * amp * 0.01;
        if (on) {
            double t = (double)(i % period) / (double)voiced;
            double env = 0.5 - 0.5 * cos(2.0 * M_PI * t);
            v += sin(2.0 * M_PI * 180.0 * i / RATE) * amp * env;
        }
        s[i] = (int16_t)v;
    }
}

/* ---- WAV inspection ---------------------------------------------------- */

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
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

static int inspect(const char *path)
{
    size_t n = 0;
    uint32_t rate = 0;
    int16_t *s = wav_load(path, &n, &rate);
    if (!s)
        return 1;

    if (rate != RATE)
        fprintf(stderr, "warning: %u Hz, not %d -- frames are not 20 ms and "
                        "the numbers below do not transfer\n", rate, RATE);

    vad_report r;
    bool speech = vad_has_speech(s, n, &r);
    printf("%s\n", path);
    printf("  %.2f s, %zu frames\n", (double)n / rate, r.frames);
    printf("  peak      %.1f%% of full scale\n", r.peak / 327.68);
    printf("  p10       %.1f dBFS\n", r.p10_db);
    printf("  p90       %.1f dBFS\n", r.p90_db);
    printf("  spread    %.1f dB   (threshold %.1f)\n",
           r.spread_db, VAD_MIN_SPREAD_DB);
    printf("  verdict   %s\n", speech ? "speech" : vad_verdict_str(r.verdict));

    free(s);
    return 0;
}

/* ---- tests -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc > 1)
        return inspect(argv[1]);

    static int16_t buf[RATE * 3];
    const size_t two_s = RATE * 2;

    rng_reset();

    /* Digital silence. Rejected on the floor, not the envelope. */
    memset(buf, 0, two_s * sizeof(*buf));
    check("digital silence", buf, two_s, false);

    /* White noise at every gain from "barely there" to "clipping". The level
     * changes; the flatness does not, which is the whole point. */
    fill_noise(buf, two_s, 300.0);
    check("white noise, 1% of full scale", buf, two_s, false);
    fill_noise(buf, two_s, 1600.0);
    check("white noise, 5% of full scale", buf, two_s, false);
    fill_noise(buf, two_s, 6500.0);
    check("white noise, 20% of full scale", buf, two_s, false);
    fill_noise(buf, two_s, 26000.0);
    check("white noise, 80% of full scale", buf, two_s, false);

    /* The real adversary: an analog input with nothing plugged into it. Level
     * wanders, clears the 2% floor, carries no voice. The first is tuned to the
     * 10.4 dB measured on the hardware this was written for, the second to half
     * again as much, so the threshold is not sitting on the edge of its own
     * evidence.
     *
     * There is a ceiling here worth knowing about: a noise floor that genuinely
     * wandered more than ~16 dB would read as speech, because by this measure
     * it is indistinguishable from speech. Nothing observed comes close -- the
     * worst real floor was 10.4 -- but the failure mode is a wasted API call
     * and a nonsense transcript, not silence, so it is the direction to watch
     * if this ever misbehaves in the field. */
    fill_wandering(buf, two_s, 2500.0, 16.0);
    check("wandering interference, dead jack", buf, two_s, false);
    fill_wandering(buf, two_s, 2500.0, 22.0);
    check("wandering interference, 50% worse", buf, two_s, false);

    /* Steady hiss with one loud transient in it -- a keyboard click, a door.
     * max - min would read that as an envelope; p90 - p10 does not. */
    fill_noise(buf, two_s, 3000.0);
    for (size_t i = RATE; i < RATE + VAD_FRAME_SAMPLES; i++)
        buf[i] = (int16_t)(rng_bipolar() * 30000.0);
    check("hiss plus a single click", buf, two_s, false);

    /* A quiet room: near-silence with the odd click. Still nothing to say. */
    fill_noise(buf, two_s, 120.0);
    for (size_t i = RATE; i < RATE + VAD_FRAME_SAMPLES; i++)
        buf[i] = (int16_t)(rng_bipolar() * 30000.0);
    check("near-silence plus a single click", buf, two_s, false);

    /* Someone talking: syllables and the gaps between them. */
    fill_syllabic(buf, two_s, 140, 90, 9000.0);
    check("syllabic envelope, loud", buf, two_s, true);
    fill_syllabic(buf, two_s, 100, 60, 2200.0);
    check("syllabic envelope, quiet but present", buf, two_s, true);
    fill_syllabic(buf, two_s, 220, 140, 5000.0);
    check("syllabic envelope, slow speaker", buf, two_s, true);

    /* Short enough that there is no envelope to judge: the floor decides. */
    fill_noise(buf, RATE / 10, 6000.0);
    check("brief burst, above the floor", buf, RATE / 10, true);
    fill_noise(buf, RATE / 10, 100.0);
    check("brief burst, below the floor", buf, RATE / 10, false);

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
