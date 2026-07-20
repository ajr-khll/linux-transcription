/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Direct typing on compositors without the virtual-keyboard protocol
 * (GNOME/Mutter, KDE/KWin), where uinput is the only way in.
 *
 * uinput injects raw keycodes below the compositor, which then interprets them
 * through the user's *active* layout. So we cannot simply pick "the key that
 * has an A on it" -- we must ask what keycode and modifier level produces the
 * character under the user's layout, and emit that. The compositor applies the
 * same layout on the way back out and the two cancel, yielding the intended
 * character.
 *
 * This is why the layout must be declared in config: we compute against it.
 * If the declared layout is not the one currently active, output is scrambled
 * -- see the caveat in the README. Layout-switchers want the clipboard backend.
 *
 * libxkbcommon holds all the layout data, so there are no tables here. */

#include "../injector.h"
#include "../log.h"
#include "../uinput_kbd.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#define MAX_CHORD_MODS 4
#define KEY_DELAY_MS   1

/* One character's keystroke: press mods, tap key, release mods. */
typedef struct {
    uint32_t codepoint;
    uint16_t key;                       /* evdev keycode */
    uint16_t mods[MAX_CHORD_MODS];      /* evdev modifier keycodes */
    uint8_t  n_mods;
} keystroke;

typedef struct {
    keystroke *map;
    size_t     n_map;
    char       layout[64];
} layout_ctx;

/* ---- building the map -------------------------------------------------- */

/* Translates one xkb modifier mask into the evdev modifier keys we must hold.
 * Returns -1 if the mask needs something we will not emit (a lock). */
static int mods_from_mask(struct xkb_keymap *km, xkb_mod_mask_t mask,
                          uint16_t *out, uint8_t *n_out)
{
    *n_out = 0;
    xkb_mod_index_t n = xkb_keymap_num_mods(km);

    for (xkb_mod_index_t i = 0; i < n; i++) {
        if (!(mask & (1u << i)))
            continue;

        const char *name = xkb_keymap_mod_get_name(km, i);
        if (!name)
            return -1;

        /* Locks are stateful: toggling CapsLock to type one character would
         * leave the user's keyboard in a different state than it started. */
        if (strcmp(name, XKB_MOD_NAME_CAPS) == 0 ||
            strcmp(name, XKB_MOD_NAME_NUM) == 0)
            return -1;

        int key;
        if (strcmp(name, XKB_MOD_NAME_SHIFT) == 0)      key = KEY_LEFTSHIFT;
        else if (strcmp(name, XKB_MOD_NAME_CTRL) == 0)  key = KEY_LEFTCTRL;
        else if (strcmp(name, XKB_MOD_NAME_ALT) == 0)   key = KEY_LEFTALT;
        else if (strcmp(name, XKB_MOD_NAME_LOGO) == 0)  key = KEY_LEFTMETA;
        else if (strcmp(name, "Mod5") == 0)             key = KEY_RIGHTALT;  /* AltGr */
        else if (strcmp(name, "Mod3") == 0)             key = KEY_RIGHTALT;  /* LevelThree */
        else return -1;

        if (*n_out >= MAX_CHORD_MODS)
            return -1;
        out[(*n_out)++] = (uint16_t)key;
    }
    return 0;
}

static bool have_codepoint(const layout_ctx *c, uint32_t cp)
{
    for (size_t i = 0; i < c->n_map; i++)
        if (c->map[i].codepoint == cp)
            return true;
    return false;
}

/* Walks every key/level of the declared layout and records which keystroke
 * produces each character. Ascending keycode order means the main row is seen
 * before the numeric keypad, so digits map to the row a human would use. */
static int build_map(layout_ctx *c, struct xkb_keymap *km)
{
    xkb_keycode_t min = xkb_keymap_min_keycode(km);
    xkb_keycode_t max = xkb_keymap_max_keycode(km);

    size_t cap = 512;
    c->map = malloc(cap * sizeof(*c->map));
    if (!c->map)
        return -1;
    c->n_map = 0;

    for (xkb_keycode_t kc = min; kc <= max; kc++) {
        if (kc < 8)
            continue;                                  /* no evdev equivalent */

        xkb_level_index_t levels = xkb_keymap_num_levels_for_key(km, kc, 0);
        for (xkb_level_index_t lvl = 0; lvl < levels; lvl++) {
            const xkb_keysym_t *syms;
            int n_syms = xkb_keymap_key_get_syms_by_level(km, kc, 0, lvl, &syms);
            if (n_syms != 1)
                continue;                              /* multi-sym: skip */

            uint32_t cp = xkb_keysym_to_utf32(syms[0]);
            if (cp == 0 || have_codepoint(c, cp))
                continue;                              /* dead key, or already mapped */

            /* Which modifiers reach this level? Take the simplest usable set. */
            xkb_mod_mask_t masks[16];
            size_t n_masks = xkb_keymap_key_get_mods_for_level(km, kc, 0, lvl,
                                                               masks, 16);
            uint16_t best[MAX_CHORD_MODS] = { 0 };
            uint8_t  best_n = 0;
            bool     found = false;

            for (size_t m = 0; m < n_masks; m++) {
                uint16_t tmp[MAX_CHORD_MODS];
                uint8_t  tmp_n;
                if (mods_from_mask(km, masks[m], tmp, &tmp_n) < 0)
                    continue;
                if (!found || tmp_n < best_n) {
                    memcpy(best, tmp, sizeof(tmp));
                    best_n = tmp_n;
                    found = true;
                }
            }
            /* Level 0 needs no modifiers even when the mask list is empty. */
            if (!found && lvl == 0) {
                best_n = 0;
                found = true;
            }
            if (!found)
                continue;

            if (c->n_map == cap) {
                cap *= 2;
                keystroke *p = realloc(c->map, cap * sizeof(*p));
                if (!p) {
                    /* The caller frees `c` but knows nothing about this
                     * allocation, so release it here. */
                    free(c->map);
                    c->map = NULL;
                    c->n_map = 0;
                    return -1;
                }
                c->map = p;
            }
            keystroke *k = &c->map[c->n_map++];
            k->codepoint = cp;
            k->key = (uint16_t)(kc - 8);               /* xkb -> evdev */
            k->n_mods = best_n;
            memcpy(k->mods, best, sizeof(best));
        }
    }
    return 0;
}

static const keystroke *lookup(const layout_ctx *c, uint32_t cp)
{
    /* The table is a few hundred entries and this runs once per character of
     * a spoken sentence; a linear scan is not worth indexing. */
    for (size_t i = 0; i < c->n_map; i++)
        if (c->map[i].codepoint == cp)
            return &c->map[i];
    return NULL;
}

/* ---- vtable ------------------------------------------------------------ */

static bool ul_probe(void)
{
    /* uinput availability is the real gate; a bad layout fails in init. */
    return true;
}

static void *ul_init(const config *cfg)
{
    if (!cfg->layout[0]) {
        log_err("uinput backend needs 'layout' set in config\n");
        return NULL;
    }

    struct xkb_context *xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb) {
        log_err("cannot create xkb context\n");
        return NULL;
    }

    const struct xkb_rule_names names = {
        .rules = NULL, .model = NULL,
        .layout = cfg->layout,
        .variant = cfg->variant[0] ? cfg->variant : NULL,
        .options = NULL,
    };
    struct xkb_keymap *km = xkb_keymap_new_from_names(xkb, &names,
                                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        log_err("cannot compile xkb layout '%s'%s%s\n", cfg->layout,
                cfg->variant[0] ? " variant " : "", cfg->variant);
        xkb_context_unref(xkb);
        return NULL;
    }

    layout_ctx *c = calloc(1, sizeof(*c));
    if (!c || build_map(c, km) < 0) {
        free(c);
        xkb_keymap_unref(km);
        xkb_context_unref(xkb);
        return NULL;
    }
    snprintf(c->layout, sizeof(c->layout), "%s", cfg->layout);

    xkb_keymap_unref(km);
    xkb_context_unref(xkb);

    if (uinput_kbd_open() < 0) {
        free(c->map);
        free(c);
        return NULL;
    }

    log_info("layout '%s': %zu characters typeable directly\n",
             c->layout, c->n_map);
    return c;
}

/* Decodes one UTF-8 sequence, advancing *p. Returns 0 at end of string. */
static uint32_t next_codepoint(const unsigned char **p)
{
    const unsigned char *s = *p;
    if (!*s)
        return 0;

    uint32_t cp;
    int extra;
    if (*s < 0x80)                { cp = *s;        extra = 0; }
    else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; extra = 1; }
    else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; extra = 2; }
    else if ((*s & 0xF8) == 0xF0) { cp = *s & 0x07; extra = 3; }
    else { *p = s + 1; return 0xFFFD; }

    s++;
    for (int i = 0; i < extra; i++, s++) {
        if ((*s & 0xC0) != 0x80) {
            *p = s;
            return 0xFFFD;
        }
        cp = (cp << 6) | (*s & 0x3F);
    }
    *p = s;
    return cp;
}

static int ul_send(void *vctx, const char *utf8)
{
    layout_ctx *c = vctx;
    const unsigned char *p = (const unsigned char *)utf8;

    size_t skipped = 0;
    uint32_t first_skipped = 0;

    for (uint32_t cp; (cp = next_codepoint(&p)) != 0; ) {
        const keystroke *k = lookup(c, cp);
        if (!k) {
            /* No key in this layout produces it. Reaching it would need a
             * dead-key or compose sequence, which this backend does not do. */
            if (!skipped++)
                first_skipped = cp;
            continue;
        }
        if (uinput_kbd_chord(k->key, (const int[]){ k->mods[0], k->mods[1],
                                                    k->mods[2], k->mods[3] },
                             k->n_mods) < 0)
            return -1;
        if (KEY_DELAY_MS)
            nanosleep(&(struct timespec){ .tv_nsec = KEY_DELAY_MS * 1000L * 1000 },
                      NULL);
    }

    if (skipped)
        log_warn("layout '%s' cannot type %zu character(s) (first: U+%04X); "
                 "they were dropped -- use the clipboard backend for full "
                 "Unicode\n", c->layout, skipped, first_skipped);
    return 0;
}

static void ul_destroy(void *vctx)
{
    layout_ctx *c = vctx;
    uinput_kbd_close();
    if (c)
        free(c->map);
    free(c);
}

const inject_backend backend_uinput_layout = {
    .name    = "uinput",
    .probe   = ul_probe,
    .init    = ul_init,
    .send    = ul_send,
    .destroy = ul_destroy,
};
