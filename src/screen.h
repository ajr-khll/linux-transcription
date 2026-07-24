/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* The record of what scribe has put on screen and not yet handed to the user.
 *
 * One thing can be replaced by another with only the tail they differ in
 * retyped -- which is how the live preview turns into the transcript, and how
 * the raw transcript turns into the cleaned-up one, without erasing and
 * retyping a whole sentence for a one-word change.
 *
 * This was the private core of live.c. It moved out because the cleanup pass
 * needs the same swap when the preview is off, and getting the erase count
 * wrong here does not smudge the screen -- it eats the user's own text to the
 * left of the caret. That is worth one tested module rather than two copies.
 *
 * NOT thread-safe, and deliberately so: exactly one thread drives these calls.
 * When the live preview runs that is the preview thread; when it does not it is
 * the worker thread. Never both at once -- main.c picks between the two paths
 * and they do not overlap. A caller that ignores this races on `shown`. */
#ifndef SCRIBE_SCREEN_H
#define SCRIBE_SCREEN_H

#include <stddef.h>

#include "injector.h"

/* Borrows `inj`, which must outlive every other call here. Resets the record
 * to empty: nothing is on screen yet. */
void screen_attach(injector *inj);

/* Moves the screen from what it shows now to `want`, erasing only the tail the
 * two do not share and typing the rest. After this the record equals `want`,
 * unless the backend dropped characters or a send failed -- in which case the
 * next call clears the ground and starts over rather than trusting a count it
 * can no longer stand behind. */
void screen_render(const char *want);

/* The text on screen belongs to the user now: forget it, so the next render
 * starts from empty and never tries to erase into the user's document. */
void screen_release(void);

/* Codepoints still ours to take back -- what a full erase would remove. */
size_t screen_owned(void);

/* Erases anything still owned and drops the injector. For shutdown, so a
 * preview or a staged transcript left on screen does not outlive the daemon. */
void screen_detach(void);

#endif
