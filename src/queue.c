/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "queue.h"
#include "log.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

struct queue {
    pcm_buffer    **slots;
    size_t          cap, head, count;
    bool            closed;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
};

void pcm_buffer_free(pcm_buffer *b)
{
    if (!b)
        return;
    free(b->samples);
    free(b);
}

queue *queue_create(size_t cap)
{
    queue *q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    q->slots = calloc(cap, sizeof(*q->slots));
    if (!q->slots) {
        free(q);
        return NULL;
    }
    q->cap = cap;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    return q;
}

int queue_push(queue *q, pcm_buffer *b)
{
    pthread_mutex_lock(&q->lock);
    if (q->closed) {
        pthread_mutex_unlock(&q->lock);
        pcm_buffer_free(b);
        return -1;
    }
    if (q->count == q->cap) {
        log_warn("transcription queue full, dropping oldest utterance\n");
        pcm_buffer_free(q->slots[q->head]);
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    q->slots[(q->head + q->count) % q->cap] = b;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

pcm_buffer *queue_pop(queue *q)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->cond, &q->lock);

    pcm_buffer *b = NULL;
    if (q->count > 0) {
        b = q->slots[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
    return b;
}

void queue_close(queue *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = true;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

void queue_destroy(queue *q)
{
    if (!q)
        return;
    for (size_t i = 0; i < q->count; i++)
        pcm_buffer_free(q->slots[(q->head + i) % q->cap]);
    free(q->slots);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
    free(q);
}
