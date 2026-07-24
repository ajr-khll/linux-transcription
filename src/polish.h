/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Semantic cleanup: an instruction model turns a raw transcript into what the
 * speaker meant to write. It drops "um" and "uh", resolves a spoken correction
 * ("Monday, uh, no Tuesday" -> "Tuesday"), and adds punctuation, without
 * rephrasing. The model runs on this machine; polish_init refuses a remote
 * endpoint so that turning cleanup on cannot start uploading what is dictated.
 *
 * Every failure keeps the raw transcript. A dead server, a timeout, an answer
 * that reads like the model ignored the instructions -- all of them return NULL
 * from polish_text, and the caller types what the transcriber heard. */
#ifndef SCRIBE_POLISH_H
#define SCRIBE_POLISH_H

#include <stdbool.h>

#include "config.h"

/* Reads the config and the word list, and opens the HTTP handle. Returns 0 when
 * cleanup is off (a no-op thereafter) or on and ready, and -1 when it is on but
 * misconfigured -- a remote or empty endpoint -- which should stop the daemon,
 * exactly as a missing API key does for the OpenAI engine. */
int polish_init(const config *cfg);

/* Whether a call to polish_text would do anything. False when cleanup is off. */
bool polish_enabled(void);

/* Returns a cleaned copy of `raw`, or NULL to keep `raw` unchanged. NULL is the
 * answer for every failure and for a reply that does not survive polish_accept.
 * Caller frees. Called only from the worker thread. */
char *polish_text(const char *raw);

void polish_shutdown(void);

/* The guard on the model's reply, split out because it is where the judgement
 * lives and it is worth testing without a server. Instruction models wander:
 * they answer the transcript instead of editing it, wrap it in "Sure, here you
 * go", or quote it. Returns a copy of `candidate` fit to use -- trimmed, and
 * with a wrapping quote pair the raw did not have removed -- or NULL when the
 * reply should be thrown away and the raw kept. Caller frees. */
char *polish_accept(const char *raw, const char *candidate);

#endif
