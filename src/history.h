/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef WHISPRD_HISTORY_H
#define WHISPRD_HISTORY_H

#include "config.h"

/* Appends transcripts to session files under the history directory, so the
 * menu has something to list. Off unless cfg->history is set: everything the
 * user dictates ends up on disk, which is a decision they should make rather
 * than inherit.
 *
 * Consecutive utterances land in one file until the speaker goes quiet for
 * HISTORY_GAP_S, which starts a new session. */
#define HISTORY_GAP_S 300

/* Creates the history directory (0700). Returns 0 when recording is off, so a
 * failure here is only ever about a directory that could not be made. */
int  history_init(const config *cfg);

/* Called from the worker thread after a successful transcript. Never fails
 * loudly: losing a history line must not disturb dictation. */
void history_append(const char *text);

void history_shutdown(void);

#endif
