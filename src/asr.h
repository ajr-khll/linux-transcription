/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* One engine per file, all with the same shape as transcribe.h. transcribe.c
 * includes this to pick between them; the rest of the daemon sees transcribe_*
 * and has no idea which engine answers.
 *
 * live.c includes it too, for the streaming engine alone. That one is not a
 * transcriber and does not appear behind transcribe_pcm: it feeds the preview,
 * which the real engine's answer then replaces. */
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

/* The streaming engine behind the live preview: a small zipformer that answers
 * while the user is still speaking. It is not the transcript of record --
 * Parakeet still decodes the sealed utterance and its answer replaces this one
 * -- so its job is to be fast and roughly right, not to be correct.
 *
 * Same build switch as Parakeet, because it is the same sherpa-onnx library. */
int   asr_stream_init(const config *cfg);

/* Opens a decoding stream for one utterance. */
int   asr_stream_begin(void);

/* Feeds one chunk and decodes what it can. Returns a newly allocated copy of
 * the text so far *only when it changed* since the last call, so a caller that
 * types the difference does nothing on the many chunks that add no words.
 * NULL means nothing new, which is the common case. */
char *asr_stream_feed(const int16_t *samples, size_t n_samples);

/* Flushes the tail the decoder is still holding and closes the stream. Returns
 * the final text, changed or not, or NULL if there is none. */
char *asr_stream_end(void);

void  asr_stream_shutdown(void);

#else

/* Without the local engine there is no streaming model either, so these stand
 * in for it and live.c compiles unchanged. Nothing calls them: transcribe_init
 * refuses `engine = parakeet` on this build and the daemon stops before
 * live_init runs. */
static inline int   asr_stream_init(const config *cfg) { (void)cfg; return -1; }
static inline int   asr_stream_begin(void) { return -1; }
static inline char *asr_stream_feed(const int16_t *s, size_t n)
{
    (void)s; (void)n; return NULL;
}
static inline char *asr_stream_end(void) { return NULL; }
static inline void  asr_stream_shutdown(void) { }

#endif

#endif
