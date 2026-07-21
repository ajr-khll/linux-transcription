/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Deciding whether an utterance is worth transcribing.
 *
 * A peak test alone only catches a source that is off. It does not catch the
 * one that hurts more: a source that is on, hissing at 5% of full scale, and
 * carrying no voice. Whisper answers that with hallucinated caption
 * boilerplate, so the failure arrives as plausible text rather than as an
 * error.
 *
 * What separates the two is shape, not level. Speech alternates between
 * syllable and pause many times a second, so its frame levels spread across
 * 15 dB or more. A noise floor holds one level and spreads across two or three.
 * Measuring that spread with p90 - p10 rather than max - min keeps a single
 * keyboard click or door slam from faking an envelope on top of steady hiss. */

#include "vad.h"

#include <math.h>
#include <stdlib.h>

/* RMS of one frame in dBFS. */
static double frame_db(const int16_t *s, size_t n)
{
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double v = s[i];
        sum += v * v;
    }
    double rms = sqrt(sum / (double)n);
    if (rms <= 0.0)
        return VAD_DB_SILENT;
    double db = 20.0 * log10(rms / 32768.0);
    return db < VAD_DB_SILENT ? VAD_DB_SILENT : db;
}

static int cmp_db(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Nearest-rank percentile of an ascending array. */
static double percentile(const double *sorted, size_t n, double p)
{
    size_t i = (size_t)(p * (double)(n - 1) + 0.5);
    return sorted[i];
}

const char *vad_verdict_str(vad_verdict v)
{
    switch (v) {
    case VAD_SPEECH:    return "speech";
    case VAD_TOO_QUIET: return "silence";
    case VAD_TOO_FLAT:  return "steady noise, no speech envelope";
    }
    return "unknown";
}

bool vad_has_speech(const int16_t *s, size_t n, vad_report *out)
{
    vad_report r = {
        .verdict = VAD_SPEECH,
        .peak = 0,
        .p10_db = VAD_DB_SILENT,
        .p90_db = VAD_DB_SILENT,
        .spread_db = 0.0,
        .frames = n / VAD_FRAME_SAMPLES,
    };

    for (size_t i = 0; i < n; i++) {
        int v = s[i] < 0 ? -s[i] : s[i];     /* int, so -32768 does not wrap */
        if (v > r.peak)
            r.peak = v;
    }

    /* Measured before the verdict rather than inside a branch, so a rejection
     * can always report the numbers it was based on. */
    bool measured = false;
    if (r.frames > 0) {
        double *db = malloc(r.frames * sizeof(*db));
        if (db) {
            for (size_t f = 0; f < r.frames; f++)
                db[f] = frame_db(s + f * VAD_FRAME_SAMPLES, VAD_FRAME_SAMPLES);
            qsort(db, r.frames, sizeof(*db), cmp_db);
            r.p10_db = percentile(db, r.frames, 0.10);
            r.p90_db = percentile(db, r.frames, 0.90);
            r.spread_db = r.p90_db - r.p10_db;
            free(db);
            measured = true;
        }
    }

    /* The floor stays a separate reason to reject: a genuinely dead source
     * should fail on being dead, not on the shape of its silence. */
    if (r.peak < VAD_SILENCE_PEAK)
        r.verdict = VAD_TOO_QUIET;
    else if (measured && r.frames >= VAD_MIN_FRAMES &&
             r.spread_db < VAD_MIN_SPREAD_DB)
        r.verdict = VAD_TOO_FLAT;

    if (out)
        *out = r;
    return r.verdict == VAD_SPEECH;
}
