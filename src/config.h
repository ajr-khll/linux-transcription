#ifndef WHISPRD_CONFIG_H
#define WHISPRD_CONFIG_H

#include <stddef.h>

#define CFG_MAX_MODS 4

typedef struct {
    int    hotkey_code;                 /* evdev code of the held key */
    int    mod_codes[CFG_MAX_MODS];     /* modifiers that must also be down */
    size_t n_mods;

    char endpoint_url[512];             /* cloud vs local is JUST this */
    char model[128];
    char api_key[256];                  /* empty for a local server */

    char backend[32];                   /* auto | wlr-vk | clipboard | x11 | uinput */
    char layout[64];                    /* xkb layout, for uinput-layout backend */

    int    paste_key;                   /* paste chord, already parsed */
    int    paste_mods[CFG_MAX_MODS];
    size_t n_paste_mods;
} config;

/* Loads `path`, or the default location when path is NULL. A missing file is
 * not an error: the built-in defaults are usable against a local server. */
int config_load(config *cfg, const char *path);

/* "KEY_RIGHTCTRL", "ctrl", "shift", "v" -> evdev code, or -1. */
int config_parse_key_name(const char *name);

#endif
