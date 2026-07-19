#ifndef WHISPRD_QUEUE_H
#define WHISPRD_QUEUE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t *samples;
    size_t   n_samples;
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
