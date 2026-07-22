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
#include "log.h"

#include <string.h>

enum engine {
    ENGINE_OPENAI,
    ENGINE_PARAKEET,
};

/* Chosen in transcribe_init and read by the worker thread thereafter. main.c
 * joins the worker before it reloads, so the two never race. */
static enum engine active = ENGINE_OPENAI;

int transcribe_init(const config *cfg)
{
    if (strcmp(cfg->engine, "openai") == 0) {
        active = ENGINE_OPENAI;
        return asr_openai_init(cfg);
    }

    if (strcmp(cfg->engine, "parakeet") == 0) {
#ifdef WITH_PARAKEET
        active = ENGINE_PARAKEET;
        return asr_parakeet_init(cfg);
#else
        /* Naming the flag matters: without it this reads as an unknown engine,
         * and the user goes looking for a typo in a word they spelled right. */
        log_err("engine = parakeet, but this build has no local engine.\n"
                "Rebuild with: ./install-parakeet.sh && make WITH_PARAKEET=1\n");
        return -1;
#endif
    }

    log_err("unknown engine '%s'; use 'openai' or 'parakeet'\n", cfg->engine);
    return -1;
}

char *transcribe_pcm(const int16_t *samples, size_t n_samples)
{
#ifdef WITH_PARAKEET
    if (active == ENGINE_PARAKEET)
        return asr_parakeet_pcm(samples, n_samples);
#endif
    return asr_openai_pcm(samples, n_samples);
}

void transcribe_shutdown(void)
{
#ifdef WITH_PARAKEET
    if (active == ENGINE_PARAKEET) {
        asr_parakeet_shutdown();
        return;
    }
#endif
    asr_openai_shutdown();
}
