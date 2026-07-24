/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* The live preview engine: a streaming zipformer decoded through sherpa-onnx's
 * online recognizer, which answers while the user is still speaking.
 *
 * Parakeet cannot do this. It is an offline model -- sherpa-onnx exposes it
 * only through SherpaOnnxOfflineRecognizer, which wants the whole utterance --
 * and NVIDIA ships no streaming export of it. Re-decoding a growing buffer
 * fakes the effect, but the cost climbs with the length of the utterance and a
 * non-causal encoder revises words far behind the tail, so the preview would
 * thrash. A model built to stream does neither.
 *
 * The trade is accuracy, and it is paid for elsewhere: this text is only ever a
 * preview. main.c replaces it with Parakeet's answer when the hotkey comes up,
 * so a wrong word here costs a few extra backspaces, not a wrong transcript.
 *
 * Threading matches asr_parakeet.c: the recognizer is a file-static, and only
 * the live thread in live.c ever calls in here.
 */
#include "asr.h"
#include "audio.h"
#include "log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sherpa-onnx/c-api/c-api.h>

/* Where install-parakeet.sh puts the streaming model. Overridable per install
 * through the Makefile, and per user through `live_model_dir` in the config. */
#ifndef SCRIBE_LIVE_MODEL_DIR
#define SCRIBE_LIVE_MODEL_DIR "/usr/local/share/scribe/models/streaming-zipformer-en"
#endif

/* The installer renames the upstream files, whose names carry the training
 * epoch and the chunk geometry, to these. That keeps the layout identical to
 * the Parakeet directory and keeps this file out of the business of tracking
 * what a model archive happened to be called. */
#define MODEL_ENCODER "encoder.int8.onnx"
#define MODEL_DECODER "decoder.int8.onnx"
#define MODEL_JOINER  "joiner.int8.onnx"
#define MODEL_TOKENS  "tokens.txt"

/* Two is enough for a 20 ms chunk against a model this size, and it leaves the
 * cores free for the Parakeet decode that follows. Raise it with `threads`. */
#define DEFAULT_THREADS 2

static const SherpaOnnxOnlineRecognizer *rec;
static const SherpaOnnxOnlineStream     *stream;

/* What `rec` was built from, so a reload can tell whether it still fits. */
static char loaded_dir[512];
static int  loaded_threads;

/* The text this engine last handed out, so feed() can stay quiet when a chunk
 * adds no words -- which is most of them. */
static char *last_text;

/* Grown once and reused. Converting every chunk would otherwise malloc and free
 * fifty times a second on the live thread. */
static float  *scratch;
static size_t  scratch_cap;

static void free_recognizer(void)
{
    if (stream)
        SherpaOnnxDestroyOnlineStream(stream);
    if (rec)
        SherpaOnnxDestroyOnlineRecognizer(rec);
    stream = NULL;
    rec = NULL;
    free(scratch);
    scratch = NULL;
    scratch_cap = 0;
    free(last_text);
    last_text = NULL;
}

/* Reports the missing file by name. A recognizer built on absent paths fails
 * with a bare NULL, which tells the user only that something is wrong. */
static int check_model_files(const char *dir)
{
    static const char *files[] = {
        MODEL_ENCODER, MODEL_DECODER, MODEL_JOINER, MODEL_TOKENS,
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[640];
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        if (access(path, R_OK) != 0) {
            log_err("live model file missing or unreadable: %s\n", path);
            log_err("run ./install-parakeet.sh, or point `live_model_dir` at a "
                    "directory that has it\n");
            return -1;
        }
    }
    return 0;
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int asr_stream_init(const config *cfg)
{
    const char *dir = cfg->live_model_dir[0] ? cfg->live_model_dir
                                             : SCRIBE_LIVE_MODEL_DIR;
    int threads = cfg->threads > 0 ? cfg->threads : DEFAULT_THREADS;

    /* Same reasoning as asr_parakeet_init: scribe-menu sends SIGHUP on every
     * settings save, and reloading ONNX graphs on each one would stall the
     * daemon. Keep the recognizer when nothing it was built from has moved. */
    if (rec && strcmp(loaded_dir, dir) == 0 && loaded_threads == threads) {
        log_dbg("live: model unchanged, keeping the loaded recognizer\n");
        return 0;
    }

    free_recognizer();

    if (check_model_files(dir) < 0)
        return -1;

    char encoder[640], decoder[640], joiner[640], tokens[640];
    snprintf(encoder, sizeof(encoder), "%s/%s", dir, MODEL_ENCODER);
    snprintf(decoder, sizeof(decoder), "%s/%s", dir, MODEL_DECODER);
    snprintf(joiner,  sizeof(joiner),  "%s/%s", dir, MODEL_JOINER);
    snprintf(tokens,  sizeof(tokens),  "%s/%s", dir, MODEL_TOKENS);

    /* Zeroed then set field by field, for the reason asr_parakeet.c gives: this
     * struct carries members for models we do not use and gains more every
     * release, so a positional initialiser would silently shift. */
    SherpaOnnxOnlineRecognizerConfig sc;
    memset(&sc, 0, sizeof(sc));

    sc.feat_config.sample_rate = AUDIO_RATE;
    sc.feat_config.feature_dim = 80;

    sc.model_config.transducer.encoder = encoder;
    sc.model_config.transducer.decoder = decoder;
    sc.model_config.transducer.joiner  = joiner;
    sc.model_config.tokens      = tokens;
    sc.model_config.num_threads = threads;
    sc.model_config.provider    = "cpu";
    sc.decoding_method          = "greedy_search";

    /* Left off deliberately. The hotkey says where an utterance starts and
     * ends; silence does not. With endpointing on, a pause for breath would
     * reset the decoder mid-sentence and the preview would start over from an
     * empty string while the user was still holding the key. */
    sc.enable_endpoint = 0;

    double t0 = now_seconds();
    rec = SherpaOnnxCreateOnlineRecognizer(&sc);
    if (!rec) {
        log_err("could not load the live model from %s\n", dir);
        return -1;
    }

    snprintf(loaded_dir, sizeof(loaded_dir), "%s", dir);
    loaded_threads = threads;

    /* The recognizer outlives every reload, so this is the only place it can be
     * released. Registered once; atexit would otherwise stack a handler per
     * model change. */
    static bool at_exit_registered;
    if (!at_exit_registered) {
        atexit(free_recognizer);
        at_exit_registered = true;
    }

    log_info("live: loaded %s in %.1fs (%d threads)\n",
             dir, now_seconds() - t0, threads);
    return 0;
}

int asr_stream_begin(void)
{
    if (!rec)
        return -1;

    /* A previous utterance that ended badly can leave one behind. Dropping it
     * here rather than leaking is the whole reason this is not an assert. */
    if (stream) {
        SherpaOnnxDestroyOnlineStream(stream);
        stream = NULL;
    }
    free(last_text);
    last_text = NULL;

    stream = SherpaOnnxCreateOnlineStream(rec);
    if (!stream) {
        log_err("live: could not create a decoding stream\n");
        return -1;
    }
    return 0;
}

/* This model emits bare uppercase words -- "AFTER EARLY NIGHTFALL" -- while
 * Parakeet, whose answer replaces it, writes "After early nightfall". Left
 * alone the two share one character, so the swap on release would take back the
 * whole line and retype it: a full flicker on every utterance.
 *
 * Folding the preview to sentence case makes the two agree for as far as the
 * words agree. On the model's own sample that cut the retraction from 103
 * characters to 3. Punctuation still differs, so a sentence with a comma in the
 * middle diverges there -- this is a cheap improvement, not a guarantee.
 *
 * ASCII only, which is all this English model produces. */
static void soften_case(char *s)
{
    bool start = true;
    for (char *p = s; *p; p++) {
        if (start && *p >= 'A' && *p <= 'Z')
            start = false;                  /* leave the opening capital */
        else if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    }
}

/* Reads whatever the decoder will say now. Returns a copy when it differs from
 * last_text, and adopts it; NULL otherwise. */
static char *take_if_changed(void)
{
    const SherpaOnnxOnlineRecognizerResult *r =
        SherpaOnnxGetOnlineStreamResult(rec, stream);
    if (!r)
        return NULL;

    char *text = strdup(r->text ? r->text : "");
    SherpaOnnxDestroyOnlineRecognizerResult(r);
    if (!text)
        return NULL;
    soften_case(text);

    if (last_text && strcmp(last_text, text) == 0) {
        free(text);
        return NULL;                        /* nothing new; the common case */
    }

    char *copy = strdup(text);
    if (!copy) {
        free(text);
        return NULL;
    }
    free(last_text);
    last_text = copy;
    return text;
}

char *asr_stream_feed(const int16_t *samples, size_t n_samples)
{
    if (!rec || !stream || n_samples == 0 || n_samples > INT32_MAX)
        return NULL;

    if (n_samples > scratch_cap) {
        float *p = realloc(scratch, n_samples * sizeof(*p));
        if (!p) {
            log_err("live: out of memory converting a chunk\n");
            return NULL;
        }
        scratch = p;
        scratch_cap = n_samples;
    }
    for (size_t i = 0; i < n_samples; i++)
        scratch[i] = (float)samples[i] / 32768.0f;

    SherpaOnnxOnlineStreamAcceptWaveform(stream, AUDIO_RATE, scratch,
                                         (int32_t)n_samples);
    while (SherpaOnnxIsOnlineStreamReady(rec, stream))
        SherpaOnnxDecodeOnlineStream(rec, stream);

    return take_if_changed();
}

char *asr_stream_end(void)
{
    if (!rec || !stream)
        return NULL;

    /* The decoder holds back the last chunk or so waiting for right context.
     * Without this the final words of every utterance would be missing from the
     * preview, and the swap to Parakeet's answer would look like a bigger
     * correction than it was. */
    SherpaOnnxOnlineStreamInputFinished(stream);
    while (SherpaOnnxIsOnlineStreamReady(rec, stream))
        SherpaOnnxDecodeOnlineStream(rec, stream);

    /* Unlike feed(), this answers whether or not the text moved: the caller is
     * closing the utterance and needs to know what is on screen. */
    char *out = NULL;
    const SherpaOnnxOnlineRecognizerResult *r =
        SherpaOnnxGetOnlineStreamResult(rec, stream);
    if (r) {
        out = strdup(r->text ? r->text : "");
        SherpaOnnxDestroyOnlineRecognizerResult(r);
        if (out)
            soften_case(out);
    }

    SherpaOnnxDestroyOnlineStream(stream);
    stream = NULL;
    free(last_text);
    last_text = NULL;
    return out;
}

/* Deliberately empty, exactly as asr_parakeet_shutdown is: main.c calls this on
 * the way to a SIGHUP reload as well as on the way out and cannot tell us
 * which, so freeing here would drop the model on every settings save.
 * free_recognizer() runs from atexit instead. */
void asr_stream_shutdown(void)
{
}
