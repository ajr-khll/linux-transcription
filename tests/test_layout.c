/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Verifies the property the uinput-layout backend depends on: if we resolve a
 * character to (keycode, modifiers) under a layout, then feed those same
 * events back through that layout, we get the original character out. That is
 * the "scrambling cancels out" claim, checked without a compositor.
 *
 * Also asserts the specific mappings a human can verify by looking at their
 * own keyboard -- 'A' is Shift+the-A-key, and on AZERTY that key is Q. */

#include "backends/uinput_layout.c"

#include <stdio.h>

static int fails;

static struct xkb_keymap *compile(struct xkb_context *ctx, const char *layout,
                                  const char *variant)
{
    const struct xkb_rule_names names = {
        .rules = NULL, .model = NULL, .layout = layout,
        .variant = variant, .options = NULL,
    };
    return xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
}

/* Replays a keystroke through a real xkb state, exactly as a compositor would
 * interpret the events uinput emits. */
static void replay(struct xkb_keymap *km, const keystroke *k, char *out, size_t n)
{
    struct xkb_state *st = xkb_state_new(km);

    for (uint8_t i = 0; i < k->n_mods; i++)
        xkb_state_update_key(st, k->mods[i] + 8, XKB_KEY_DOWN);

    xkb_state_key_get_utf8(st, k->key + 8, out, n);

    for (uint8_t i = k->n_mods; i > 0; i--)
        xkb_state_update_key(st, k->mods[i - 1] + 8, XKB_KEY_UP);

    xkb_state_unref(st);
}

static void round_trip(const char *layout, const char *variant, const char *text)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = compile(ctx, layout, variant);
    if (!km) {
        printf("SKIP  layout %s: not available on this system\n", layout);
        xkb_context_unref(ctx);
        return;
    }

    layout_ctx c = { 0 };
    build_map(&c, km);

    char got[1024] = { 0 };
    size_t len = 0, missing = 0;
    const unsigned char *p = (const unsigned char *)text;
    for (uint32_t cp; (cp = next_codepoint(&p)) != 0; ) {
        const keystroke *k = lookup(&c, cp);
        if (!k) {
            missing++;
            continue;
        }
        char buf[8] = { 0 };
        replay(km, k, buf, sizeof(buf));
        len += (size_t)snprintf(got + len, sizeof(got) - len, "%s", buf);
    }

    int ok = strcmp(got, text) == 0 && missing == 0;
    printf("%s  %-10s %zu keys mapped | %s\n", ok ? "PASS" : "FAIL",
           variant ? variant : layout, c.n_map, text);
    if (!ok) {
        printf("      got back: %s\n", got);
        if (missing)
            printf("      %zu character(s) had no mapping\n", missing);
        fails++;
    }

    free(c.map);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
}

/* Asserts a mapping a human can check against a physical keyboard. */
static void expect_key(const char *layout, uint32_t cp, int key, int n_mods,
                       const char *desc)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = compile(ctx, layout, NULL);
    layout_ctx c = { 0 };
    build_map(&c, km);

    const keystroke *k = lookup(&c, cp);
    int ok = k && k->key == key && k->n_mods == n_mods;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", desc);
    if (!ok) {
        if (k)
            printf("      got keycode %u with %u mods, wanted %d with %d\n",
                   k->key, k->n_mods, key, n_mods);
        else
            printf("      no mapping found\n");
        fails++;
    }
    free(c.map);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
}

/* Asserts a character is NOT typeable, pinning a documented limitation. */
static void unmappable(const char *layout, uint32_t cp, const char *desc)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = compile(ctx, layout, NULL);
    layout_ctx c = { 0 };
    build_map(&c, km);
    int ok = lookup(&c, cp) == NULL;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", desc);
    if (!ok)
        fails++;
    free(c.map);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
}

int main(void)
{
    round_trip("us", NULL, "Hello, world! The quick brown fox: 0123456789");
    round_trip("us", NULL, "punctuation \"quotes\" 'apos' #$%&*()[]{}<>@~`^|\\/+=-_;:?");
    round_trip("gb", NULL, "Sterling costs money.");
    round_trip("fr", NULL, "Bonjour, ca va bien! 123");
    round_trip("fr", NULL, "accents on AZERTY: \u00e9\u00e8\u00e7\u00e0\u00f9");
    round_trip("de", NULL, "Gr\u00fc\u00dfe aus M\u00fcnchen: \u00e4\u00f6\u00fc\u00c4\u00d6\u00dc 42");
    round_trip("us", "intl", "Hello from the international layout");

    /* QWERTY: 'A' is Shift + the physical A key (evdev KEY_A = 30). */
    expect_key("us", 'A', KEY_A, 1, "us: 'A' is Shift + KEY_A");
    expect_key("us", 'a', KEY_A, 0, "us: 'a' is KEY_A, no modifiers");
    /* AZERTY: the letter A sits on the physical Q key (evdev KEY_Q = 16). */
    expect_key("fr", 'a', KEY_Q, 0, "fr: 'a' is on the physical Q key (AZERTY)");
    expect_key("fr", 'q', KEY_A, 0, "fr: 'q' is on the physical A key (AZERTY)");
    /* German: 'z' and 'y' are swapped relative to QWERTY (QWERTZ). */
    expect_key("de", 'z', KEY_Y, 0, "de: 'z' is on the physical Y key (QWERTZ)");

    /* Live mode retracts a preview by asking the injector to type U+0008.
     * xkbcommon reports XKB_KEY_BackSpace as that codepoint, so the build_map
     * walk records KEY_BACKSPACE like any other character and ul_send needs no
     * special case. Asserted on two layouts because the whole retraction path
     * rests on it. */
    expect_key("us", '\b', KEY_BACKSPACE, 0, "us: U+0008 is KEY_BACKSPACE, unmodified");
    expect_key("fr", '\b', KEY_BACKSPACE, 0, "fr: U+0008 is KEY_BACKSPACE, unmodified");

    /* Known limitation, asserted so it cannot regress silently: characters no
     * key produces (curly quotes, em dash -- which Whisper does emit) have no
     * mapping on a plain us layout and are dropped by ul_send. */
    unmappable("us", 0x201C, "us: curly quote U+201C has no mapping (documented gap)");
    unmappable("us", 0x2014, "us: em dash U+2014 has no mapping (documented gap)");

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
