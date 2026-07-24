/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef SCRIBE_CONFIG_H
#define SCRIBE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define CFG_MAX_MODS 4

typedef struct {
    int    hotkey_code;                 /* evdev code of the held key */
    int    mod_codes[CFG_MAX_MODS];     /* modifiers that must also be down */
    size_t n_mods;

    /* Which transcriber runs. The local one has no URL at all, so there is
     * nothing in endpoint_url to judge it by -- hence a key of its own. */
    char engine[32];                    /* openai | parakeet */
    char model_dir[512];                /* parakeet: empty = installed default */
    int  threads;                       /* parakeet: decode threads; 0 = default */

    /* Type a running preview while the key is held, then replace it with the
     * engine's answer on release. Off unless asked for. Needs the local engine,
     * a backend that can backspace and a hotkey that is not a modifier, so
     * live_init has the last word -- this key only says the user wants it. */
    bool live;
    char live_model_dir[512];           /* empty = installed default */

    /* Clean up each transcript with a local instruction model: drop fillers,
     * resolve spoken self-corrections, add punctuation. Off unless asked for,
     * and refused at startup against a non-local endpoint -- see polish.c. */
    bool cleanup;
    char cleanup_endpoint_url[512];     /* OpenAI-compatible chat server, on this host */
    char cleanup_model[128];
    int  cleanup_timeout_ms;            /* how long to wait before keeping the raw text */
    char vocabulary_file[512];          /* empty = ~/.config/scribe/vocabulary.txt */

    char endpoint_url[512];             /* openai: server to POST to */
    char model[128];
    char api_key[256];                  /* empty for a local server */

    char source[256];                   /* pulse source name; empty = default */

    bool history;                       /* keep transcripts on disk */
    char history_dir[512];              /* empty = XDG data dir */

    bool audio_cues;                    /* play start/stop/error tones */

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

/* Writes the file config_load would read into `out`: `override` when it is set,
 * the XDG default otherwise. The file watcher needs the same answer config_load
 * arrives at, and must not have to guess it a second time. */
void config_resolve_path(char *out, size_t n, const char *override);

/* "KEY_RIGHTCTRL", "ctrl", "shift", "v" -> evdev code, or -1. */
int config_parse_key_name(const char *name);

/* Whether holding the hotkey keeps a modifier down -- because the hotkey is one
 * (the default, KEY_RIGHTCTRL, is), or because the chord names one.
 *
 * This decides whether the live preview can run. Typing while the user holds
 * Ctrl turns every letter into a shortcut in whatever window has focus: Ctrl+V
 * pastes, Ctrl+W closes it, Ctrl+Q quits it. Injecting after release, which is
 * what scribe did before the preview existed, never met this. */
bool config_hotkey_holds_modifier(const config *cfg);

/* Renders the hotkey chord back into "KEY_LEFTALT+KEY_Z" form for logging. */
void config_hotkey_desc(const config *cfg, char *buf, size_t n);

/* Same, for the paste chord. */
void config_paste_chord_desc(const config *cfg, char *buf, size_t n);

#endif
