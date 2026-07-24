/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Drives the preview's redraw against a fake screen.
 *
 * This is the part of live mode that can do real damage. Every character it
 * takes back is a BackSpace delivered to whatever window has focus, so an
 * erase counted wrong does not garble scribe's own output -- it eats the text
 * the user typed before they ever pressed the hotkey. The tests below therefore
 * check the screen contents after each step, and separately check that the
 * preview never erases more than it put there.
 *
 * The fake injector stands in for a real backend and can be told to drop
 * characters, which is what the uinput layout does for anything it has no key
 * for. That is the case where what we typed and what we meant to type come
 * apart, and where erasing by the length of the string would be wrong.
 *
 * live.c is pulled in whole so the redraw can be called directly, without a
 * thread, a model or a compositor.
 */

#include "live.c"

static int fails;

/* ---- the fake screen --------------------------------------------------- */

static char   screen[4096];
static size_t screen_len;                   /* bytes */
static size_t overrun;                      /* erases past the start */

/* Codepoints this backend refuses to type, as a layout would. */
static const char *undeliverable = "";

static void screen_reset(void)
{
    screen[0] = '\0';
    screen_len = 0;
    overrun = 0;
    undeliverable = "";
}

size_t injector_utf8_len(const char *utf8)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p; p++)
        if ((*p & 0xC0) != 0x80)
            n++;
    return n;
}

bool injector_can_erase(const injector *i)
{
    (void)i;
    return true;
}

int injector_send(injector *i, const char *utf8, size_t *landed)
{
    (void)i;
    if (landed)
        *landed = 0;

    for (const unsigned char *p = (const unsigned char *)utf8; *p; ) {
        size_t w = 1;
        while (p[w] && ((unsigned char)p[w] & 0xC0) == 0x80)
            w++;

        if (w == 1 && strchr(undeliverable, (char)*p)) {
            p += w;
            continue;                       /* no key produces it; dropped */
        }
        memcpy(screen + screen_len, p, w);
        screen_len += w;
        screen[screen_len] = '\0';
        if (landed)
            (*landed)++;
        p += w;
    }
    return 0;
}

int injector_erase(injector *i, size_t n_codepoints)
{
    (void)i;
    for (size_t k = 0; k < n_codepoints; k++) {
        if (screen_len == 0) {
            /* Past the start of what the preview typed. On a real screen this
             * is the user's own text going away. */
            overrun++;
            continue;
        }
        do {
            screen_len--;
        } while (screen_len > 0 &&
                 ((unsigned char)screen[screen_len] & 0xC0) == 0x80);
        screen[screen_len] = '\0';
    }
    return 0;
}

/* ---- helpers ----------------------------------------------------------- */

/* Puts the preview back in the state live_init leaves it. */
static void preview_reset(void)
{
    free(shown);
    shown = strdup("");
    typed = 0;
    synced = true;
    inj = (injector *)&screen;              /* never dereferenced by the fakes */
    screen_reset();
}

static void check(const char *what, const char *want)
{
    int ok = strcmp(screen, want) == 0 && overrun == 0;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        printf("      screen: \"%s\"\n      wanted: \"%s\"\n", screen, want);
        if (overrun)
            printf("      ate %zu character(s) of the user's own text\n", overrun);
        fails++;
    }
}

/* ---- tests ------------------------------------------------------------- */

/* The ordinary case: a transducer that only ever appends. */
static void test_growth(void)
{
    preview_reset();
    render("After");
    render("After early");
    render("After early nightfall");
    check("a growing preview appends", "After early nightfall");
}

/* The tail changes, which is what a revision looks like. */
static void test_revision(void)
{
    preview_reset();
    render("the squalid quarter of the brothel");
    render("the squalid quarter of the brothers");
    check("a revised tail is corrected", "the squalid quarter of the brothers");
}

/* The engine's answer is shorter than the preview. */
static void test_shrink(void)
{
    preview_reset();
    render("hello there world");
    render("hello");
    check("a shorter answer erases the rest", "hello");
}

/* The whole line changes. Nothing shared means everything goes. */
static void test_total_replacement(void)
{
    preview_reset();
    render("wrong from the start");
    render("right from the start");
    check("a fully different answer replaces it", "right from the start");
}

/* Multi-byte characters must be erased whole. Counting bytes here would leave
 * half a character on screen and eat one more than intended. */
static void test_utf8(void)
{
    preview_reset();
    render("café naïve résumé");
    render("café naïve");
    check("multi-byte characters erase whole", "café naïve");

    preview_reset();
    render("日本語のテキスト");
    render("日本語");
    check("CJK erases whole", "日本語");

    /* The shared prefix ends inside a character: "é" and "è" share their first
     * byte. Backing off to the boundary is what stops a half-erase. */
    preview_reset();
    render("café");
    render("cafè");
    check("a prefix ending mid-character backs off", "cafè");
}

/* An empty commit, which is how a rejected utterance arrives. */
static void test_retract_all(void)
{
    preview_reset();
    render("this was never really said");
    apply_commit("");
    check("an empty commit takes the whole preview back", "");

    /* And having committed, the preview must have forgotten it: the next
     * utterance starting must not try to erase text that is now the user's. */
    render("next one");
    check("a commit forgets what it typed", "next one");
}

/* A commit leaves nothing owed, so the following utterance starts clean. */
static void test_commit_then_next(void)
{
    preview_reset();
    render("hello wurld");
    apply_commit("Hello, world.");
    check("the commit swaps in the transcript", "Hello, world.");

    render("and again");
    apply_commit("And again.");
    check("a second utterance appends rather than overwriting",
          "Hello, world.And again.");
}

/* The backend cannot type some of what it was given -- the uinput layout does
 * this for curly quotes and em dashes. What is on screen is then shorter than
 * what we asked for, and the next redraw has to erase the real number, not the
 * intended one. This is the case that eats the user's text when it is wrong. */
static void test_dropped_characters(void)
{
    preview_reset();
    undeliverable = "xyz";

    render("abcxyzdef");
    check("undeliverable characters are simply missing", "abcdef");

    /* The preview is out of step now. The next redraw must clear exactly what
     * landed -- six characters, not nine -- and start again. */
    undeliverable = "";
    render("abcxyzdefghi");
    check("the next redraw clears only what landed", "abcxyzdefghi");
}

/* A commit arriving while the preview is out of step. */
static void test_desync_then_commit(void)
{
    preview_reset();
    undeliverable = "q";

    render("a quick brown fox");
    undeliverable = "";
    apply_commit("A quick brown fox.");
    check("a commit recovers from a dropped character", "A quick brown fox.");
}

/* The preview types while the hotkey is still down, so a hotkey that holds a
 * modifier turns every character into a shortcut in the focused window: Ctrl+V
 * pastes, Ctrl+W closes it, Ctrl+Q quits it. This is the one live-mode failure
 * that destroys the user's work rather than scribe's output, so the refusal is
 * pinned here. The default hotkey is KEY_RIGHTCTRL, which means the default
 * config must refuse. */
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

static void test_modifier_hotkeys(void)
{
    expect_modifier_verdict("KEY_RIGHTCTRL", true,
                            "the default hotkey refuses the preview");
    expect_modifier_verdict("KEY_LEFTALT", true, "left alt refuses");
    expect_modifier_verdict("KEY_RIGHTSHIFT", true, "right shift refuses");
    expect_modifier_verdict("KEY_LEFTMETA", true, "super refuses");
    expect_modifier_verdict("alt+z", true, "a chord with a modifier refuses");
    expect_modifier_verdict("KEY_F13", false, "a spare function key allows it");
    expect_modifier_verdict("KEY_SCROLLLOCK", false, "scroll lock allows it");
}

int main(void)
{
    test_modifier_hotkeys();
    test_growth();
    test_revision();
    test_shrink();
    test_total_replacement();
    test_utf8();
    test_retract_all();
    test_commit_then_next();
    test_dropped_characters();
    test_desync_then_commit();

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
