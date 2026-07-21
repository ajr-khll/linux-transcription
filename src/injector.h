/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef SCRIBE_INJECTOR_H
#define SCRIBE_INJECTOR_H

#include <stdbool.h>

#include "config.h"

typedef struct injector injector;

/* Detects the session and picks a backend, unless cfg->backend forces one. */
injector *injector_init(const config *cfg);
int       injector_send(injector *inj, const char *utf8_text);
void      injector_destroy(injector *inj);

/* Backend vtable. Each backend file exports one of these. */
typedef struct {
    const char *name;
    bool  (*probe)(void);                    /* can this run right now? */
    void *(*init)(const config *cfg);        /* NULL on failure */
    int   (*send)(void *ctx, const char *utf8);
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
