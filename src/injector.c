/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
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
#ifdef WITH_UINPUT_LAYOUT
    &backend_uinput_layout,
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

size_t injector_utf8_len(const char *utf8)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p; p++)
        if ((*p & 0xC0) != 0x80)
            n++;
    return n;
}

int injector_send(injector *inj, const char *utf8_text, size_t *typed)
{
    if (typed)
        *typed = 0;
    if (!inj || !utf8_text || !*utf8_text)
        return 0;

    size_t n = 0;
    int rc = inj->be->send(inj->ctx, utf8_text, &n);
    if (typed)
        *typed = n;
    return rc;
}

bool injector_can_erase(const injector *inj)
{
    return inj && inj->be && inj->be->erases;
}

int injector_erase(injector *inj, size_t n_codepoints)
{
    if (!inj || n_codepoints == 0)
        return 0;
    if (!inj->be->erases) {
        log_err("backend '%s' cannot erase\n", inj->be->name);
        return -1;
    }

    /* U+0008 needs no special case in either typing backend: xkbcommon reports
     * XKB_KEY_BackSpace as that codepoint, so the uinput backend already found
     * KEY_BACKSPACE while walking the layout, and the wlr-vk backend puts the
     * keysym straight into the keymap it builds per send. */
    while (n_codepoints > 0) {
        char buf[65];
        size_t take = n_codepoints < sizeof(buf) - 1 ? n_codepoints : sizeof(buf) - 1;
        memset(buf, '\b', take);
        buf[take] = '\0';

        size_t erased = 0;
        int rc = inj->be->send(inj->ctx, buf, &erased);
        /* A short erase is worse than a failed one: the caller's idea of what
         * is on screen is now wrong, and erasing again would eat the user's own
         * text. Say so and let it stop. */
        if (rc < 0 || erased != take) {
            log_err("erased %zu of %zu character(s); giving up on the rest\n",
                    erased, take);
            return -1;
        }
        n_codepoints -= take;
    }
    return 0;
}

void injector_destroy(injector *inj)
{
    if (!inj)
        return;
    if (inj->be && inj->ctx)
        inj->be->destroy(inj->ctx);
    free(inj);
}
