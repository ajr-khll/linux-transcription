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
