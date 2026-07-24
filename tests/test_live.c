/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* The gate the live preview stands behind.
 *
 * The redraw itself -- diffing what is on screen and erasing only the tail that
 * changed -- moved to screen.c, and test_screen.c drives it. What is left here
 * is the precondition live_init checks before it ever runs: a hotkey that holds
 * a modifier cannot drive the preview, because the preview types while the key
 * is still down, so every letter would become a shortcut in the focused window.
 * The default hotkey is KEY_RIGHTCTRL, so the default config must refuse. This
 * is the one live-mode failure that destroys the user's work rather than
 * scribe's own output, so the refusal is pinned here.
 */
#include "config.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect_modifier_verdict(const char *chord, bool want, const char *desc)
{
    config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.hotkey_code = config_parse_key_name(chord);

    /* A chord's modifiers count too: holding "alt+z" keeps Alt down. */
    const char *plus = strchr(chord, '+');
    if (plus) {
        char mod[64];
        snprintf(mod, sizeof(mod), "%.*s", (int)(plus - chord), chord);
        cfg.mod_codes[0] = config_parse_key_name(mod);
        cfg.n_mods = 1;
        cfg.hotkey_code = config_parse_key_name(plus + 1);
    }

    bool got = config_hotkey_holds_modifier(&cfg);
    printf("%s  %s\n", got == want ? "PASS" : "FAIL", desc);
    if (got != want) {
        printf("      %s: holds a modifier = %s, wanted %s\n",
               chord, got ? "yes" : "no", want ? "yes" : "no");
        fails++;
    }
}

int main(void)
{
    expect_modifier_verdict("KEY_RIGHTCTRL", true,
                            "the default hotkey refuses the preview");
    expect_modifier_verdict("KEY_LEFTALT", true, "left alt refuses");
    expect_modifier_verdict("KEY_RIGHTSHIFT", true, "right shift refuses");
    expect_modifier_verdict("KEY_LEFTMETA", true, "super refuses");
    expect_modifier_verdict("alt+z", true, "a chord with a modifier refuses");
    expect_modifier_verdict("KEY_F13", false, "a spare function key allows it");
    expect_modifier_verdict("KEY_SCROLLLOCK", false, "scroll lock allows it");

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
