/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef SCRIBE_VAD_H
#define SCRIBE_VAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 20 ms at 16 kHz: long enough for a settled RMS, short enough to fit inside a
 * single syllable. */
#define VAD_FRAME_SAMPLES  320

/* ~2% of full scale. A live mic in a quiet room peaks well above this; an
 * unplugged input's noise floor sits well below it. */
#define VAD_SILENCE_PEAK   655

/* Speech swings between syllable and pause; a noise floor wanders at most.
 * Measured, not guessed, on the hardware this was written for:
 *
 *   USB mic, idle                     2.2 dB
 *   onboard analog in, nothing plugged in
 *                                    10.4 dB   <- the one that caused this
 *   speech, quietest 4 s stretch     24.1 dB
 *   speech, 2 s slice                27.9 dB
 *
 * An unconnected analog jack is not flat hiss: it picks up interference that
 * drifts over a 10 dB range, which is why a threshold near the 8 dB this
 * started at would have passed the exact source it was meant to catch. 16 sits
 * 5.6 dB above the worst noise and 8.1 dB below the quietest speech. Re-measure
 * with `test_vad <file.wav>` before moving it. */
#define VAD_MIN_SPREAD_DB  16.0

/* Below this many frames there is not enough of an envelope to judge, so only
 * the absolute floor applies. The preroll alone is 250 ms, so a real utterance
 * always clears this. */
#define VAD_MIN_FRAMES     10

/* Stands in for the -infinity dBFS of digital silence. */
#define VAD_DB_SILENT      (-100.0)

typedef enum {
    VAD_SPEECH,         /* worth sending to the transcriber */
    VAD_TOO_QUIET,      /* never cleared the absolute floor: dead source */
    VAD_TOO_FLAT,       /* loud enough, but no envelope: noise, not speech */
} vad_verdict;

typedef struct {
    vad_verdict verdict;
    int    peak;            /* largest |sample|, 0..32768 */
    double p10_db;          /* quiet frames, dBFS */
    double p90_db;          /* loud frames, dBFS */
    double spread_db;       /* p90 - p10 */
    size_t frames;
} vad_report;

/* True when `s` looks like someone talking rather than a noise floor. `out`
 * receives the measurements behind the verdict and may be NULL.
 *
 * Deliberately free of any PulseAudio dependency: it takes samples and returns
 * a judgement, so it can be run against a recorded WAV offline. */
bool vad_has_speech(const int16_t *s, size_t n, vad_report *out);

/* The verdict as a short phrase, for logs. */
const char *vad_verdict_str(vad_verdict v);

#endif
