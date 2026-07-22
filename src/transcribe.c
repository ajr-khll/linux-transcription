/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Picks an engine and forwards to it. The rest of the daemon calls
 * transcribe_* and never learns which one answered. */
#include "transcribe.h"
#include "asr.h"

int transcribe_init(const config *cfg)
{
    return asr_openai_init(cfg);
}

char *transcribe_pcm(const int16_t *samples, size_t n_samples)
{
    return asr_openai_pcm(samples, n_samples);
}

void transcribe_shutdown(void)
{
    asr_openai_shutdown();
}
