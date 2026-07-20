/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef WHISPRD_CONFIG_H
#define WHISPRD_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define CFG_MAX_MODS 4

typedef struct {
    int    hotkey_code;                 /* evdev code of the held key */
    int    mod_codes[CFG_MAX_MODS];     /* modifiers that must also be down */
    size_t n_mods;

    char endpoint_url[512];             /* cloud vs local is JUST this */
    char model[128];
    char api_key[256];                  /* empty for a local server */

    char source[256];                   /* pulse source name; empty = default */

    bool history;                       /* keep transcripts on disk */
    char history_dir[512];              /* empty = XDG data dir */

    char backend[32];                   /* auto | wlr-vk | clipboard | x11 | uinput */
    char layout[64];                    /* xkb layout, for uinput-layout backend */
    char variant[64];                   /* optional xkb variant, e.g. "intl" */

    int    paste_key;                   /* paste chord, already parsed */
    int    paste_mods[CFG_MAX_MODS];
    size_t n_paste_mods;
} config;

/* Loads `path`, or the default location when path is NULL. A missing file is
 * not an error: the built-in defaults are usable against a local server. */
int config_load(config *cfg, const char *path);

/* "KEY_RIGHTCTRL", "ctrl", "shift", "v" -> evdev code, or -1. */
int config_parse_key_name(const char *name);

/* Renders the hotkey chord back into "KEY_LEFTALT+KEY_Z" form for logging. */
void config_hotkey_desc(const config *cfg, char *buf, size_t n);

/* Same, for the paste chord. */
void config_paste_chord_desc(const config *cfg, char *buf, size_t n);

#endif
