/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* NVIDIA Parakeet TDT 0.6B v3, decoded in this process through sherpa-onnx.
 * Nothing leaves the machine and there is no second process to supervise.
 *
 * The recognizer is a file-static, exactly as the curl handle is in
 * asr_openai.c. Only worker() in main.c ever calls in here, and main.c joins
 * that thread before it reloads, so one thread touches this and there is no
 * locking. */
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

/* Where install-parakeet.sh puts the model. Overridable per install through
 * the Makefile, and per user through `model_dir` in the config. */
#ifndef SCRIBE_MODEL_DIR
#define SCRIBE_MODEL_DIR "/usr/local/share/scribe/models/parakeet-tdt-0.6b-v3-int8"
#endif

/* Parakeet is a transducer: three graphs plus the token table. All four must
 * be present or the recognizer comes back NULL with nothing said about why. */
#define MODEL_ENCODER "encoder.int8.onnx"
#define MODEL_DECODER "decoder.int8.onnx"
#define MODEL_JOINER  "joiner.int8.onnx"
#define MODEL_TOKENS  "tokens.txt"

/* Enough to keep a 0.6B model busy without starving the desktop it is running
 * on. Raise it with `threads` in the config. */
#define DEFAULT_THREADS 4

static const SherpaOnnxOfflineRecognizer *rec;

/* What `rec` was built from, so a reload can tell whether it still fits. */
static char loaded_dir[512];
static int  loaded_threads;

static void free_recognizer(void)
{
    if (rec)
        SherpaOnnxDestroyOfflineRecognizer(rec);
    rec = NULL;
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
            log_err("model file missing or unreadable: %s\n", path);
            log_err("run ./install-parakeet.sh, or point `model_dir` at a "
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

int asr_parakeet_init(const config *cfg)
{
    const char *dir = cfg->model_dir[0] ? cfg->model_dir : SCRIBE_MODEL_DIR;
    int threads = cfg->threads > 0 ? cfg->threads : DEFAULT_THREADS;

    /* scribe-menu sends SIGHUP on every settings save, and main.c answers a
     * SIGHUP by tearing the stack down and standing it back up. For a curl
     * handle that costs nothing. Reloading 640 MB of ONNX graphs on each save
     * would stall the daemon for seconds at a time, so keep the recognizer
     * when nothing it was built from has changed. */
    if (rec && strcmp(loaded_dir, dir) == 0 && loaded_threads == threads) {
        log_dbg("parakeet: model unchanged, keeping the loaded recognizer\n");
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

    /* Zeroed, then set field by field. This struct carries two dozen members
     * for models we do not use -- whisper, canary, qwen3 -- and gains more
     * every release, so a positional initialiser would silently shift the
     * moment sherpa-onnx adds one. */
    SherpaOnnxOfflineRecognizerConfig sc;
    memset(&sc, 0, sizeof(sc));

    sc.feat_config.sample_rate = AUDIO_RATE;
    sc.feat_config.feature_dim = 80;

    sc.model_config.transducer.encoder = encoder;
    sc.model_config.transducer.decoder = decoder;
    sc.model_config.transducer.joiner  = joiner;
    sc.model_config.tokens      = tokens;
    /* Without this sherpa-onnx guesses from the graph and gets Parakeet
     * wrong. */
    sc.model_config.model_type  = "nemo_transducer";
    sc.model_config.num_threads = threads;
    sc.model_config.provider    = "cpu";
    sc.decoding_method          = "greedy_search";

    double t0 = now_seconds();
    rec = SherpaOnnxCreateOfflineRecognizer(&sc);
    if (!rec) {
        log_err("could not load the Parakeet model from %s\n", dir);
        return -1;
    }

    snprintf(loaded_dir, sizeof(loaded_dir), "%s", dir);
    loaded_threads = threads;

    /* The recognizer outlives every reload, so this is the only place it can
     * be released. Registered once; atexit would otherwise stack a handler per
     * model change. */
    static bool at_exit_registered;
    if (!at_exit_registered) {
        atexit(free_recognizer);
        at_exit_registered = true;
    }

    /* Seconds, and users notice. Say so rather than let startup look hung. */
    log_info("parakeet: loaded %s in %.1fs (%d threads)\n",
             dir, now_seconds() - t0, threads);
    return 0;
}

char *asr_parakeet_pcm(const int16_t *samples, size_t n_samples)
{
    return asr_parakeet_pcm_at(samples, n_samples, AUDIO_RATE);
}

char *asr_parakeet_pcm_at(const int16_t *samples, size_t n_samples, int rate)
{
    if (!rec || n_samples == 0)
        return NULL;

    /* sherpa-onnx counts samples in an int32_t. Utterances are seconds long,
     * so this cannot fire in practice -- it is here so that if it ever does,
     * it does not wrap into a negative length. */
    if (n_samples > INT32_MAX)
        return NULL;

    float *f = malloc(n_samples * sizeof(*f));
    if (!f)
        return NULL;
    for (size_t i = 0; i < n_samples; i++)
        f[i] = (float)samples[i] / 32768.0f;

    const SherpaOnnxOfflineStream *stream = SherpaOnnxCreateOfflineStream(rec);
    if (!stream) {
        log_err("parakeet: could not create a decoding stream\n");
        free(f);
        return NULL;
    }

    SherpaOnnxAcceptWaveformOffline(stream, rate, f, (int32_t)n_samples);
    SherpaOnnxDecodeOfflineStream(rec, stream);
    free(f);

    char *text = NULL;
    const SherpaOnnxOfflineRecognizerResult *r =
        SherpaOnnxGetOfflineStreamResult(stream);
    if (!r) {
        log_err("parakeet: decoding produced no result\n");
    } else {
        if (r->text)
            text = strdup(r->text);
        SherpaOnnxDestroyOfflineRecognizerResult(r);
    }
    SherpaOnnxDestroyOfflineStream(stream);

    return text;
}

/* Deliberately empty -- see the reload comment in asr_parakeet_init. main.c
 * calls this on the way to a SIGHUP reload as well as on the way out, and it
 * cannot tell us which, so freeing here would drop the model on every settings
 * save. free_recognizer() runs from atexit instead. */
void asr_parakeet_shutdown(void)
{
}
