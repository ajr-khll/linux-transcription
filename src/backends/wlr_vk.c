/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Direct typing via zwp_virtual_keyboard_v1 (wlroots: Hyprland, Sway, river,
 * Wayfire). We upload our own keymap in which each character we need to type
 * sits alone at level 1 of its own key, so the user's active layout is
 * irrelevant and no modifier gymnastics are required.
 *
 * The manager global is absent on GNOME/Mutter and KDE/KWin, so probe() walks
 * the registry rather than trusting XDG_SESSION_TYPE=wayland. */

#include "../injector.h"
#include "../log.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"

/* xkb keycodes run 9..254, so this many distinct characters per keymap. */
#define MAX_KEYS      246
#define KEY_DELAY_MS  1

typedef struct {
    struct wl_display                      *dpy;
    struct wl_registry                     *reg;
    struct wl_seat                         *seat;
    struct zwp_virtual_keyboard_manager_v1 *mgr;
    struct zwp_virtual_keyboard_v1         *kbd;
} vk_ctx;

/* ---- registry ---------------------------------------------------------- */

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t version)
{
    vk_ctx *c = data;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !c->seat) {
        c->seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                   version < 7 ? version : 7);
    } else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        c->mgr = wl_registry_bind(reg, name,
                                  &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
    (void)version;
}

static void on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

static bool connect_and_bind(vk_ctx *c)
{
    c->dpy = wl_display_connect(NULL);
    if (!c->dpy)
        return false;
    c->reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(c->reg, &registry_listener, c);
    wl_display_roundtrip(c->dpy);
    return c->mgr != NULL && c->seat != NULL;
}

static void disconnect(vk_ctx *c)
{
    if (c->kbd)
        zwp_virtual_keyboard_v1_destroy(c->kbd);
    if (c->mgr)
        zwp_virtual_keyboard_manager_v1_destroy(c->mgr);
    if (c->seat)
        wl_seat_destroy(c->seat);
    if (c->reg)
        wl_registry_destroy(c->reg);
    if (c->dpy)
        wl_display_disconnect(c->dpy);
    memset(c, 0, sizeof(*c));
}

/* ---- keymap ------------------------------------------------------------ */

/* Decodes UTF-8 into codepoints. Returns a malloc'd array, or NULL. */
static uint32_t *utf8_decode(const char *s, size_t *out_n)
{
    size_t cap = strlen(s) + 1, n = 0;
    uint32_t *cps = malloc(cap * sizeof(*cps));
    if (!cps)
        return NULL;

    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        int extra;
        if (*p < 0x80)            { cp = *p;        extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
        else { p++; continue; }               /* stray continuation byte */

        p++;
        bool ok = true;
        for (int i = 0; i < extra; i++, p++) {
            if ((*p & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (*p & 0x3F);
        }
        if (ok)
            cps[n++] = cp;
    }
    *out_n = n;
    return cps;
}

static xkb_keysym_t keysym_for(uint32_t cp)
{
    switch (cp) {
    case '\n': return XKB_KEY_Return;
    case '\t': return XKB_KEY_Tab;
    default:   break;
    }
    xkb_keysym_t ks = xkb_utf32_to_keysym(cp);
    if (ks == XKB_KEY_NoSymbol)
        ks = 0x01000000u | cp;      /* the Unicode keysym convention */
    return ks;
}

/* Builds an xkb keymap placing distinct[i] alone on keycode 9+i.
 *
 * snprintf returns what it *would* have written, so accumulating it blindly
 * lets `len` run past `cap` -- and `cap - len` is unsigned, so the next call
 * would be handed a length near SIZE_MAX and write off the end of the heap.
 * The budget below is comfortable (measured worst case is ~66 bytes per key
 * against 64 plus 4 KB of slack), so this never trips today; the check is here
 * so that a longer keysym name than any that exists now cannot turn a truncated
 * keymap into a heap overflow. */
static char *build_keymap(const uint32_t *distinct, size_t n)
{
    size_t cap = 4096 + n * 96;
    char *km = malloc(cap);
    if (!km)
        return NULL;

    size_t len = 0;

/* Appends to km, or bails out if the buffer would not hold it. */
#define APPEND(...)                                                           \
    do {                                                                      \
        int _w = snprintf(km + len, cap - len, __VA_ARGS__);                  \
        if (_w < 0 || (size_t)_w >= cap - len) {                              \
            log_err("keymap buffer too small for %zu characters\n", n);       \
            free(km);                                                         \
            return NULL;                                                      \
        }                                                                     \
        len += (size_t)_w;                                                    \
    } while (0)

    APPEND("xkb_keymap {\n"
           "xkb_keycodes \"(scribe)\" {\n"
           "  minimum = 8;\n"
           "  maximum = 255;\n");

    for (size_t i = 0; i < n; i++)
        APPEND("  <K%zu> = %zu;\n", i, i + 9);

    APPEND("};\n"
           "xkb_types \"(scribe)\" { include \"complete\" };\n"
           "xkb_compatibility \"(scribe)\" { include \"complete\" };\n"
           "xkb_symbols \"(scribe)\" {\n");

    for (size_t i = 0; i < n; i++) {
        char name[64];
        xkb_keysym_get_name(keysym_for(distinct[i]), name, sizeof(name));
        APPEND("  key <K%zu> { [ %s ] };\n", i, name);
    }

    APPEND("};\n};\n");
#undef APPEND

    return km;
}

static int upload_keymap(vk_ctx *c, const uint32_t *distinct, size_t n)
{
    char *km = build_keymap(distinct, n);
    if (!km)
        return -1;
    size_t size = strlen(km) + 1;        /* compositor mmaps including the NUL */

    int fd = memfd_create("scribe-keymap", MFD_CLOEXEC);
    if (fd < 0) {
        log_err("memfd_create: %s\n", strerror(errno));
        free(km);
        return -1;
    }
    if (write(fd, km, size) != (ssize_t)size) {
        log_err("writing keymap: %s\n", strerror(errno));
        close(fd);
        free(km);
        return -1;
    }
    free(km);

    zwp_virtual_keyboard_v1_keymap(c->kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                   fd, (uint32_t)size);
    /* Our keymap needs no modifiers; make sure none are latched. */
    zwp_virtual_keyboard_v1_modifiers(c->kbd, 0, 0, 0, 0);
    wl_display_roundtrip(c->dpy);
    close(fd);
    return 0;
}

/* ---- typing ------------------------------------------------------------ */

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void tap(vk_ctx *c, uint32_t keycode)
{
    zwp_virtual_keyboard_v1_key(c->kbd, now_ms(), keycode,
                                WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(c->kbd, now_ms(), keycode,
                                WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_display_flush(c->dpy);
    if (KEY_DELAY_MS)
        nanosleep(&(struct timespec){ .tv_nsec = KEY_DELAY_MS * 1000L * 1000 }, NULL);
}

static size_t index_of(const uint32_t *a, size_t n, uint32_t v)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] == v)
            return i;
    return (size_t)-1;
}

static int vk_send(void *vctx, const char *utf8)
{
    vk_ctx *c = vctx;

    size_t n_cps;
    uint32_t *cps = utf8_decode(utf8, &n_cps);
    if (!cps)
        return -1;

    int rc = 0;
    size_t i = 0;
    while (i < n_cps) {
        /* Take as many characters as fit one keymap's worth of distinct ones. */
        uint32_t distinct[MAX_KEYS];
        size_t nd = 0, j = i;
        while (j < n_cps) {
            if (index_of(distinct, nd, cps[j]) == (size_t)-1) {
                if (nd == MAX_KEYS)
                    break;
                distinct[nd++] = cps[j];
            }
            j++;
        }

        if (upload_keymap(c, distinct, nd) < 0) {
            rc = -1;
            break;
        }
        for (size_t k = i; k < j; k++)
            tap(c, (uint32_t)(index_of(distinct, nd, cps[k]) + 1));

        i = j;
    }

    wl_display_roundtrip(c->dpy);
    free(cps);
    return rc;
}

/* ---- vtable ------------------------------------------------------------ */

static bool vk_probe(void)
{
    vk_ctx probe = { 0 };
    bool ok = connect_and_bind(&probe);
    disconnect(&probe);
    return ok;
}

static void *vk_init(const config *cfg)
{
    (void)cfg;
    vk_ctx *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;

    if (!connect_and_bind(c)) {
        disconnect(c);
        free(c);
        return NULL;
    }
    c->kbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(c->mgr, c->seat);
    if (!c->kbd) {
        disconnect(c);
        free(c);
        return NULL;
    }
    return c;
}

static void vk_destroy(void *vctx)
{
    vk_ctx *c = vctx;
    disconnect(c);
    free(c);
}

const inject_backend backend_wlr_vk = {
    .name    = "wlr-vk",
    .probe   = vk_probe,
    .init    = vk_init,
    .send    = vk_send,
    .destroy = vk_destroy,
};
