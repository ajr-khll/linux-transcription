/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef SCRIBE_CUE_H
#define SCRIBE_CUE_H

#include "config.h"

/* Short audio cues so the user can tell, without looking at anything, which
 * state whisprd is in. */
enum cue_kind {
    CUE_START,      /* hotkey down: listening */
    CUE_STOP,       /* hotkey up: captured, uploading */
    CUE_ERROR,      /* nothing usable came back */
};

/* Reads cfg->audio_cues and locates a player. Safe to call again after a
 * config reload. */
void cue_init(const config *cfg);

/* Plays the cue and returns at once; it never blocks the caller and leaves no
 * child behind. A no-op when cues are off or no player was found. */
void cue_play(enum cue_kind kind);

#endif
