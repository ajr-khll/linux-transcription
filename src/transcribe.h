/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef WHISPRD_TRANSCRIBE_H
#define WHISPRD_TRANSCRIBE_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

int transcribe_init(const config *cfg);

/* POSTs the samples as an in-memory WAV and returns the transcript, malloc'd
 * UTF-8, or NULL on failure. Caller frees. */
char *transcribe_pcm(const int16_t *samples, size_t n_samples);

void transcribe_shutdown(void);

#endif
