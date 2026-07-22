/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* One engine per file, all three with the same shape as transcribe.h. Only
 * transcribe.c includes this: the rest of the daemon sees transcribe_* and has
 * no idea which engine answers. */
#ifndef SCRIBE_ASR_H
#define SCRIBE_ASR_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* OpenAI (or any compatible endpoint) over HTTP. Always compiled in. */
int   asr_openai_init(const config *cfg);
char *asr_openai_pcm(const int16_t *samples, size_t n_samples);
void  asr_openai_shutdown(void);

#ifdef WITH_PARAKEET
/* Parakeet TDT via sherpa-onnx, in this process. */
int   asr_parakeet_init(const config *cfg);
char *asr_parakeet_pcm(const int16_t *samples, size_t n_samples);
void  asr_parakeet_shutdown(void);

/* Same, for samples that are not at AUDIO_RATE -- sherpa-onnx resamples
 * internally. The daemon never needs this, since capture is pinned to
 * AUDIO_RATE, but the smoke test decodes whatever WAV it is handed and the
 * model ships samples at 22 and 24 kHz. */
char *asr_parakeet_pcm_at(const int16_t *samples, size_t n_samples, int rate);
#endif

#endif
