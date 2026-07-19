#include "audio.h"
#include "log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/error.h>
#include <pulse/simple.h>

#define CHUNK_SAMPLES  320                      /* 20 ms */
#define PREROLL_MS     250
#define PREROLL_N      (AUDIO_RATE * PREROLL_MS / 1000)
#define MAX_UTTERANCE  (AUDIO_RATE * 120)       /* hard cap: 2 minutes */

static pa_simple      *stream;
static queue          *out_queue;
static pthread_t       thread;
static atomic_bool     capturing;
static atomic_bool     running;

/* Rolling window of the most recent audio. Seeded into an utterance on the
 * rising edge so the leading consonant is never lost to the gap between the
 * physical keypress and us observing it. */
static int16_t preroll[PREROLL_N];
static size_t  preroll_pos;
static bool    preroll_full;

static int16_t *utt;
static size_t   utt_n, utt_cap;

static void preroll_write(const int16_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        preroll[preroll_pos] = src[i];
        preroll_pos = (preroll_pos + 1) % PREROLL_N;
        if (preroll_pos == 0)
            preroll_full = true;
    }
}

static int utt_append(const int16_t *src, size_t n)
{
    if (utt_n + n > MAX_UTTERANCE)
        n = utt_n < MAX_UTTERANCE ? MAX_UTTERANCE - utt_n : 0;
    if (n == 0)
        return 0;

    if (utt_n + n > utt_cap) {
        size_t cap = utt_cap ? utt_cap * 2 : AUDIO_RATE;   /* start at 1 s */
        while (cap < utt_n + n)
            cap *= 2;
        int16_t *p = realloc(utt, cap * sizeof(*p));
        if (!p) {
            log_err("out of memory growing utterance buffer\n");
            return -1;
        }
        utt = p;
        utt_cap = cap;
    }
    memcpy(utt + utt_n, src, n * sizeof(*src));
    utt_n += n;
    return 0;
}

static void utt_begin(void)
{
    utt_n = 0;
    size_t n = preroll_full ? PREROLL_N : preroll_pos;
    if (n == 0)
        return;
    /* Oldest sample first. */
    size_t start = preroll_full ? preroll_pos : 0;
    for (size_t i = 0; i < n; i++) {
        int16_t s = preroll[(start + i) % PREROLL_N];
        utt_append(&s, 1);
    }
}

static void utt_seal(void)
{
    if (utt_n < AUDIO_RATE / 10) {          /* < 100 ms: a stray tap */
        log_dbg("utterance too short (%zu samples), discarded\n", utt_n);
        utt_n = 0;
        return;
    }

    pcm_buffer *b = malloc(sizeof(*b));
    if (!b)
        return;
    b->samples = malloc(utt_n * sizeof(int16_t));
    if (!b->samples) {
        free(b);
        return;
    }
    memcpy(b->samples, utt, utt_n * sizeof(int16_t));
    b->n_samples = utt_n;
    utt_n = 0;

    log_dbg("sealed utterance: %.2f s\n", (double)b->n_samples / AUDIO_RATE);
    queue_push(out_queue, b);
}

static void *capture_loop(void *arg)
{
    (void)arg;
    int16_t chunk[CHUNK_SAMPLES];
    bool was_capturing = false;

    while (atomic_load(&running)) {
        int err;
        if (pa_simple_read(stream, chunk, sizeof(chunk), &err) < 0) {
            log_err("pa_simple_read: %s\n", pa_strerror(err));
            break;
        }

        bool now = atomic_load(&capturing);
        if (now && !was_capturing)
            utt_begin();
        if (now)
            utt_append(chunk, CHUNK_SAMPLES);
        else if (was_capturing)
            utt_seal();

        preroll_write(chunk, CHUNK_SAMPLES);
        was_capturing = now;
    }
    return NULL;
}

int audio_init(const config *cfg, queue *out)
{
    (void)cfg;
    out_queue = out;

    /* No resampling anywhere: ask the server for exactly what we ship. */
    const pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = AUDIO_RATE,
        .channels = 1,
    };
    /* Small fragments keep pa_simple_read from blocking long, so shutdown is
     * responsive without any extra wakeup plumbing. */
    const pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .fragsize = CHUNK_SAMPLES * sizeof(int16_t),
        .tlength = (uint32_t)-1,
        .prebuf = (uint32_t)-1,
        .minreq = (uint32_t)-1,
    };

    int err;
    stream = pa_simple_new(NULL, "whisprd", PA_STREAM_RECORD, NULL,
                           "voice transcription", &ss, NULL, &attr, &err);
    if (!stream) {
        log_err("cannot open capture stream: %s\n", pa_strerror(err));
        return -1;
    }

    atomic_store(&running, true);
    if (pthread_create(&thread, NULL, capture_loop, NULL) != 0) {
        log_err("cannot start capture thread\n");
        pa_simple_free(stream);
        stream = NULL;
        return -1;
    }
    log_info("capture stream open (%d Hz mono S16)\n", AUDIO_RATE);
    return 0;
}

void audio_set_capturing(bool on)
{
    atomic_store(&capturing, on);
}

void audio_shutdown(void)
{
    if (!stream)
        return;
    atomic_store(&running, false);
    pthread_join(thread, NULL);
    pa_simple_free(stream);
    stream = NULL;
    free(utt);
    utt = NULL;
    utt_cap = utt_n = 0;
}
