/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef SCRIBE_QUEUE_H
#define SCRIBE_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t *samples;
    size_t   n_samples;

    /* The utterance ended without anything worth transcribing -- too short, or
     * no speech in it. `samples` is NULL and nothing will be decoded.
     *
     * It still has to travel down the queue rather than being dropped where it
     * was judged, because the live preview may already have typed words for it
     * and only the worker retracts them. Judging happens on the capture thread,
     * which must not block; the worker can. */
    bool     rejected;
} pcm_buffer;

void pcm_buffer_free(pcm_buffer *b);

typedef struct queue queue;

queue *queue_create(size_t cap);
void   queue_destroy(queue *q);

/* Takes ownership of `b`. On overflow the oldest entry is dropped so a slow
 * transcription can never stall capture. */
int    queue_push(queue *q, pcm_buffer *b);

/* Blocks until an entry is available. Returns NULL once the queue is closed
 * and drained, which is how the worker thread learns to exit. */
pcm_buffer *queue_pop(queue *q);

void   queue_close(queue *q);

#endif
