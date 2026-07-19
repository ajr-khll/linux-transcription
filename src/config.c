/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "config.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libevdev/libevdev.h>

int log_verbose = 0;

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return s;
}

/* Strip a trailing `# comment`. Only '#' at the start or preceded by
 * whitespace counts, so a '#' inside a value (a URL fragment, an API key)
 * survives. */
static void strip_comment(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p == '#' && (p == s || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            return;
        }
    }
}

int config_parse_key_name(const char *name)
{
    if (!name || !*name)
        return -1;

    /* Full evdev name: libevdev owns the whole table. */
    int code = libevdev_event_code_from_name(EV_KEY, name);
    if (code >= 0)
        return code;

    /* Friendly aliases, so paste_chord can read as `ctrl+shift+v`. */
    static const struct { const char *alias; int code; } aliases[] = {
        { "ctrl",  KEY_LEFTCTRL  }, { "control", KEY_LEFTCTRL  },
        { "shift", KEY_LEFTSHIFT }, { "alt",     KEY_LEFTALT   },
        { "super", KEY_LEFTMETA  }, { "meta",    KEY_LEFTMETA  },
        { "win",   KEY_LEFTMETA  }, { "altgr",   KEY_RIGHTALT  },
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (strcasecmp(name, aliases[i].alias) == 0)
            return aliases[i].code;

    /* Bare "v" / "V" / "7" -> KEY_V / KEY_7. */
    if (strlen(name) == 1) {
        char buf[16];
        snprintf(buf, sizeof(buf), "KEY_%c", toupper((unsigned char)name[0]));
        code = libevdev_event_code_from_name(EV_KEY, buf);
        if (code >= 0)
            return code;
    }
    return -1;
}

/* Parses "MOD+MOD+KEY" into a trailing key plus its required modifiers. */
static int parse_chord(const char *spec, int *key_out, int *mods, size_t *n_mods)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);

    *n_mods = 0;
    *key_out = -1;

    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        tok = trim(tok);
        if (!*tok)
            continue;
        int code = config_parse_key_name(tok);
        if (code < 0) {
            log_err("unknown key name '%s' in chord '%s'\n", tok, spec);
            return -1;
        }
        /* The last token is the key; everything before it is a modifier. */
        if (*key_out >= 0) {
            if (*n_mods >= CFG_MAX_MODS) {
                log_err("too many modifiers in chord '%s'\n", spec);
                return -1;
            }
            mods[(*n_mods)++] = *key_out;
        }
        *key_out = code;
    }
    if (*key_out < 0) {
        log_err("empty chord '%s'\n", spec);
        return -1;
    }
    return 0;
}

void config_hotkey_desc(const config *cfg, char *buf, size_t n)
{
    size_t off = 0;
    for (size_t i = 0; i < cfg->n_mods; i++) {
        const char *m = libevdev_event_code_get_name(EV_KEY, (unsigned)cfg->mod_codes[i]);
        off += (size_t)snprintf(buf + off, n - off, "%s+", m ? m : "?");
        if (off >= n)
            return;
    }
    const char *k = libevdev_event_code_get_name(EV_KEY, (unsigned)cfg->hotkey_code);
    snprintf(buf + off, n - off, "%s", k ? k : "?");
}

static void default_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%s/whisprd/config.ini", xdg);
    else
        snprintf(out, n, "%s/.config/whisprd/config.ini", getenv("HOME") ? getenv("HOME") : ".");
}

int config_load(config *cfg, const char *path)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->hotkey_code = KEY_RIGHTCTRL;
    snprintf(cfg->endpoint_url, sizeof(cfg->endpoint_url), "http://127.0.0.1:8080/v1");
    snprintf(cfg->model, sizeof(cfg->model), "whisper-1");
    snprintf(cfg->backend, sizeof(cfg->backend), "auto");
    snprintf(cfg->layout, sizeof(cfg->layout), "us");
    cfg->paste_key = KEY_V;
    cfg->paste_mods[0] = KEY_LEFTCTRL;
    cfg->n_paste_mods = 1;

    char buf[512];
    if (!path) {
        default_path(buf, sizeof(buf));
        path = buf;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        log_info("no config at %s, using defaults\n", path);
        return 0;
    }

    char line[1024];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        strip_comment(line);
        char *s = trim(line);
        if (!*s)
            continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            log_warn("%s:%d: not a key=value line, ignored\n", path, lineno);
            continue;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcmp(key, "hotkey") == 0) {
            if (parse_chord(val, &cfg->hotkey_code, cfg->mod_codes, &cfg->n_mods) < 0)
                rc = -1;
        } else if (strcmp(key, "paste_chord") == 0) {
            if (parse_chord(val, &cfg->paste_key, cfg->paste_mods, &cfg->n_paste_mods) < 0)
                rc = -1;
        } else if (strcmp(key, "endpoint_url") == 0) {
            snprintf(cfg->endpoint_url, sizeof(cfg->endpoint_url), "%s", val);
        } else if (strcmp(key, "model") == 0) {
            snprintf(cfg->model, sizeof(cfg->model), "%s", val);
        } else if (strcmp(key, "api_key") == 0) {
            snprintf(cfg->api_key, sizeof(cfg->api_key), "%s", val);
        } else if (strcmp(key, "source") == 0) {
            snprintf(cfg->source, sizeof(cfg->source), "%s", val);
        } else if (strcmp(key, "variant") == 0) {
            snprintf(cfg->variant, sizeof(cfg->variant), "%s", val);
        } else if (strcmp(key, "backend") == 0) {
            snprintf(cfg->backend, sizeof(cfg->backend), "%s", val);
        } else if (strcmp(key, "layout") == 0) {
            snprintf(cfg->layout, sizeof(cfg->layout), "%s", val);
        } else {
            log_warn("%s:%d: unknown key '%s', ignored\n", path, lineno, key);
        }
    }
    fclose(f);

    /* Trailing slash would produce a double slash in the request URL. */
    size_t ulen = strlen(cfg->endpoint_url);
    while (ulen > 0 && cfg->endpoint_url[ulen - 1] == '/')
        cfg->endpoint_url[--ulen] = '\0';

    return rc;
}
