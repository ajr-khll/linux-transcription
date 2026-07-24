/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* The live preview: types words into the focused window while the hotkey is
 * still down, then takes them back and types the real transcript on release.
 *
 * Everything here is a no-op when the preview is off, so callers do not need to
 * ask first. live_init decides, and says why in the log when the answer is no.
 */
#ifndef SCRIBE_LIVE_H
#define SCRIBE_LIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "injector.h"

/* Starts the preview thread and loads the streaming model. Returns true when
 * the preview is on. `inj` must outlive it, and is borrowed, not owned. */
bool live_init(const config *cfg, injector *inj);

/* Whether the preview is running. main.c asks so it knows whether to inject the
 * final transcript itself or hand it to live_commit. */
bool live_active(void);

/* Called from the capture thread on the hotkey edges. Neither blocks. */
void live_begin(void);
void live_close(void);

/* Called from the capture thread for every chunk while the key is held. Copies
 * and returns; the decoding happens on the preview thread. */
void live_feed(const int16_t *samples, size_t n_samples);

/* Replaces the preview with `final_text`, which may be "" for an utterance that
 * produced nothing -- the preview is retracted either way, so no caller can
 * leave stale words behind by forgetting a case.
 *
 * Called from the worker thread, and blocks until the swap is on screen. That
 * is deliberate: the preview thread is the only thread that touches the
 * injector, so waiting for it is what keeps two threads from typing at once. */
void live_commit(const char *final_text);

/* Puts `text` on screen but keeps owning it, so a following commit replaces
 * only the tail that changed rather than erasing and retyping the lot. Used for
 * the raw transcript while the cleanup model is still thinking. Returns a
 * generation token to hand back to live_commit_staged; also blocks until the
 * text is on screen, and for the same reason live_commit does. */
unsigned live_stage(const char *text);

/* Replaces the staged text with `final_text` and hands it to the user -- unless
 * a newer utterance has begun since `gen` was issued by live_stage, in which
 * case the raw text already on screen stands and the correction is dropped.
 * Blocks until the swap (or the drop) is done. */
void live_commit_staged(const char *final_text, unsigned gen);

void live_shutdown(void);

#endif
