/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef SCRIBE_INJECTOR_H
#define SCRIBE_INJECTOR_H

#include <stdbool.h>

#include "config.h"

#include <stddef.h>

typedef struct injector injector;

/* Detects the session and picks a backend, unless cfg->backend forces one. */
injector *injector_init(const config *cfg);
void      injector_destroy(injector *inj);

/* Types `utf8_text`. Returns 0, or -1 if the backend reported a failure.
 *
 * `typed`, when not NULL, receives the number of codepoints that actually
 * reached the screen. That is not always the length of the string: the uinput
 * backend drops characters its layout cannot produce, and a failure part-way
 * through still leaves everything before it typed. Anything that later wants to
 * erase what it wrote has to count in these units, because one BackSpace
 * removes one codepoint -- so `typed` is set on the failure path too. */
int injector_send(injector *inj, const char *utf8_text, size_t *typed);

/* Erases the last `n_codepoints` characters. Returns 0, or -1 if the backend
 * failed or could not erase them all -- in which case the caller no longer
 * knows what is on screen and should stop assuming. */
int injector_erase(injector *inj, size_t n_codepoints);

/* Whether this backend can take text back. False for the clipboard, which
 * pastes rather than types: a U+0008 in the clipboard is a control character,
 * not an erase. */
bool injector_can_erase(const injector *inj);

/* Codepoints in a UTF-8 string, counting the bytes that are not continuation
 * bytes. Shared because backends report their work in these units. */
size_t injector_utf8_len(const char *utf8);

/* Backend vtable. Each backend file exports one of these. */
typedef struct {
    const char *name;
    bool  erases;                            /* can it take text back? */
    bool  (*probe)(void);                    /* can this run right now? */
    void *(*init)(const config *cfg);        /* NULL on failure */
    /* 0 or -1, and sets *typed to the codepoints that landed either way. */
    int   (*send)(void *ctx, const char *utf8, size_t *typed);
    void  (*destroy)(void *ctx);
} inject_backend;

#ifdef WITH_WLR_VK
extern const inject_backend backend_wlr_vk;
#endif
#ifdef WITH_UINPUT_LAYOUT
extern const inject_backend backend_uinput_layout;
#endif
extern const inject_backend backend_clipboard;

#endif
