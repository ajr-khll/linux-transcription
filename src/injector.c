/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "injector.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

struct injector {
    const inject_backend *be;
    void                 *ctx;
};

/* Preference order for auto-detection. The first whose probe() succeeds wins,
 * so direct-typing backends are tried before the clipboard fallback. */
static const inject_backend *const candidates[] = {
#ifdef WITH_WLR_VK
    &backend_wlr_vk,
#endif
    &backend_clipboard,
};

static const inject_backend *find_named(const char *name)
{
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        if (strcmp(candidates[i]->name, name) == 0)
            return candidates[i];
    return NULL;
}

injector *injector_init(const config *cfg)
{
    injector *inj = calloc(1, sizeof(*inj));
    if (!inj)
        return NULL;

    if (strcmp(cfg->backend, "auto") != 0) {
        const inject_backend *be = find_named(cfg->backend);
        if (!be) {
            log_err("unknown or not-compiled-in backend '%s'\n", cfg->backend);
            free(inj);
            return NULL;
        }
        if (!be->probe())
            log_warn("backend '%s' was forced but its probe failed; "
                     "trying it anyway\n", be->name);
        inj->ctx = be->init(cfg);
        if (!inj->ctx) {
            log_err("backend '%s' failed to initialise\n", be->name);
            free(inj);
            return NULL;
        }
        inj->be = be;
        log_info("injection backend: %s (forced)\n", be->name);
        return inj;
    }

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const inject_backend *be = candidates[i];
        if (!be->probe()) {
            log_dbg("backend %s: probe failed\n", be->name);
            continue;
        }
        void *ctx = be->init(cfg);
        if (!ctx) {
            log_warn("backend %s probed but failed to initialise, "
                     "falling through\n", be->name);
            continue;
        }
        inj->be = be;
        inj->ctx = ctx;
        log_info("injection backend: %s\n", be->name);
        return inj;
    }

    log_err("no usable injection backend\n");
    free(inj);
    return NULL;
}

int injector_send(injector *inj, const char *utf8_text)
{
    if (!inj || !utf8_text || !*utf8_text)
        return 0;
    return inj->be->send(inj->ctx, utf8_text);
}

void injector_destroy(injector *inj)
{
    if (!inj)
        return;
    if (inj->be && inj->ctx)
        inj->be->destroy(inj->ctx);
    free(inj);
}
