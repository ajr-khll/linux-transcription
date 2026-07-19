/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef WHISPRD_AUDIO_H
#define WHISPRD_AUDIO_H

#include <stdbool.h>

#include "config.h"
#include "queue.h"

#define AUDIO_RATE 16000    /* exactly what Whisper wants: never resample */

/* Opens a persistent 16 kHz mono S16 capture stream and starts the capture
 * thread. Sealed utterances are pushed onto `out`. */
int  audio_init(const config *cfg, queue *out);

/* Called from the input thread on hotkey edges. Non-blocking. */
void audio_set_capturing(bool on);

void audio_shutdown(void);

typedef struct {
    char name[256];
    char desc[256];
    bool monitor;
} audio_source;

/* Returns a malloc'd array of capture sources; caller frees. */
audio_source *audio_enumerate_sources(size_t *n_out);

/* Records briefly from one source and returns its peak sample (0..32767),
 * or -1 if the source could not be opened. */
int audio_measure_peak(const char *source, int ms);

/* Prints every capture source with a short live level measurement, so a user
 * can pick their microphone by watching which one actually responds rather
 * than guessing from device names. Returns 0 on success. */
int audio_list_sources(void);

#endif
