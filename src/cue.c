/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* A rising pair of tones when the hotkey goes down, a falling pair when it
 * comes up, a low double beep when nothing usable came back. That is enough to
 * dictate without watching the screen.
 *
 * whisprd links no audio-output library. It already needs pulseaudio-utils for
 * capture diagnostics, so the tone is synthesised here and the raw PCM piped to
 * `paplay --raw`. No bundled sound files, no new dependency, and tones we
 * control. */

#include "cue.h"
#include "log.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CUE_RATE 44100
#define CUE_MAX  (CUE_RATE / 2)         /* half a second dwarfs any cue */

static bool enabled;
static char player[512];                /* absolute path, so exec needs no PATH */

/* Fills `out` with the path to paplay, or returns false. Resolving it once here
 * means cue_play can exec an absolute path: execvp searches PATH and may malloc,
 * neither of which is safe between fork and exec in a threaded process. */
static bool find_player(char *out, size_t n)
{
    const char *path = getenv("PATH");
    if (!path || !*path)
        return false;
    char *dup = strdup(path);
    if (!dup)
        return false;
    bool found = false;
    for (char *dir = strtok(dup, ":"); dir; dir = strtok(NULL, ":")) {
        snprintf(out, n, "%s/paplay", dir);
        if (access(out, X_OK) == 0) {
            found = true;
            break;
        }
    }
    free(dup);
    return found;
}

void cue_init(const config *cfg)
{
    enabled = false;
    if (!cfg->audio_cues)
        return;
    /* paplay ships in pulseaudio-utils, which whisprd already needs, but a
     * headless box may not have it. Check once, so we neither spam a failing
     * exec on every keypress nor stay quiet about why there is no sound. */
    if (!find_player(player, sizeof(player))) {
        log_warn("audio_cues is on but paplay was not found; install "
                 "pulseaudio-utils, or set audio_cues = off\n");
        return;
    }
    enabled = true;
}

/* Appends a `ms`-long tone at `freq` to buf, from `off`, with a short attack
 * and a longer release so neither edge clicks. Returns the new length. */
static size_t tone(int16_t *buf, size_t off, double freq, int ms)
{
    size_t n = (size_t)CUE_RATE * ms / 1000;
    if (off + n > CUE_MAX)
        n = CUE_MAX - off;
    double dur = ms / 1000.0;
    for (size_t i = 0; i < n; i++) {
        double t = i / (double)CUE_RATE;
        double env = 1.0;
        if (t < 0.005)
            env = t / 0.005;                    /* 5 ms attack */
        else if (t > dur * 0.6)
            env = (dur - t) / (dur * 0.4);      /* taper over the last 40% */
        if (env < 0.0)
            env = 0.0;
        double s = sin(2.0 * M_PI * freq * t) * env * 0.18;
        buf[off + i] = (int16_t)(s * 32767.0);
    }
    return off + n;
}

/* Writes the cue into buf and returns its length in samples. */
static size_t synth(enum cue_kind kind, int16_t *buf)
{
    switch (kind) {
    case CUE_START:                                     /* ascending */
        return tone(buf, tone(buf, 0, 330.0, 70), 440.0, 90);
    case CUE_STOP:                                      /* descending */
        return tone(buf, tone(buf, 0, 440.0, 70), 330.0, 90);
    case CUE_ERROR: {                                   /* low double beep */
        size_t n = tone(buf, 0, 175.0, 90);
        size_t gap = (size_t)CUE_RATE * 40 / 1000;
        if (n + gap < CUE_MAX) {
            memset(buf + n, 0, gap * sizeof(*buf));
            n += gap;
        }
        return tone(buf, n, 175.0, 90);
    }
    }
    return 0;
}

void cue_play(enum cue_kind kind)
{
    if (!enabled)
        return;

    /* Build the PCM before forking. sin() is not safe to call between fork and
     * exec in a threaded process, and CUE_START/CUE_STOP run on the input
     * thread; the finished buffer reaches the player through the fork. */
    int16_t pcm[CUE_MAX];
    size_t n = synth(kind, pcm);
    if (n == 0)
        return;

    /* Double-fork so the player reparents to init. The daemon waits only on the
     * middle child, which exits at once, so cue_play never blocks on playback
     * and never leaves a zombie behind. */
    pid_t mid = fork();
    if (mid < 0)
        return;
    if (mid > 0) {
        waitpid(mid, NULL, 0);              /* returns immediately */
        return;
    }

    pid_t worker = fork();
    if (worker < 0)
        _exit(127);
    if (worker > 0)
        _exit(0);                           /* orphan the worker to init */

    int fds[2];
    if (pipe(fds) < 0)
        _exit(127);

    pid_t proc = fork();
    if (proc < 0)
        _exit(127);
    if (proc == 0) {
        close(fds[1]);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        execl(player, "paplay", "--raw", "--format=s16le",
              "--rate=44100", "--channels=1", (char *)NULL);
        _exit(127);
    }

    close(fds[0]);
    const char *p = (const char *)pcm;
    size_t total = n * sizeof(int16_t), off = 0;
    while (off < total) {
        ssize_t w = write(fds[1], p + off, total - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;                          /* player gone: nothing to do */
        }
        off += (size_t)w;
    }
    close(fds[1]);
    waitpid(proc, NULL, 0);
    _exit(0);
}
