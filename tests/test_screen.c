/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Drives screen.c against a stub injector that remembers what is on screen and
 * counts what got erased. The thing worth testing is that a swap erases only
 * the tail that changed, and that a backend which cannot produce a character
 * still leaves the erase count matching what landed -- because erasing one too
 * many does not smudge the screen, it eats the user's own text. */
#include "screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

/* screen.c's log_dbg refers to it; nothing here turns it on. */
int log_verbose;

/* ---- the stub injector -------------------------------------------------- */

/* The screen as the user would see it, in UTF-8. screen.c must never assume
 * this matches its own record: that is the whole point of counting landed
 * codepoints rather than requested ones. */
static char   real[256];
static size_t erased_total;             /* codepoints erased since reset */
static char   drop;                     /* a byte the "backend" cannot type; 0 = none */

static void stub_reset(void)
{
    real[0] = '\0';
    erased_total = 0;
    drop = 0;
}

size_t injector_utf8_len(const char *utf8)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p; p++)
        if ((*p & 0xC0) != 0x80)
            n++;
    return n;
}

int injector_send(injector *inj, const char *utf8, size_t *typed)
{
    (void)inj;
    size_t landed = 0;
    size_t len = strlen(real);
    for (const char *p = utf8; *p; p++) {
        if (drop && *p == drop)
            continue;                   /* backend cannot produce this one */
        real[len++] = *p;
        if (((unsigned char)*p & 0xC0) != 0x80)
            landed++;                   /* one more codepoint reached the screen */
    }
    real[len] = '\0';
    if (typed)
        *typed = landed;
    return 0;
}

int injector_erase(injector *inj, size_t n)
{
    (void)inj;
    size_t len = strlen(real);
    while (n > 0 && len > 0) {
        len--;
        while (len > 0 && ((unsigned char)real[len] & 0xC0) == 0x80)
            len--;                      /* step back over a whole codepoint */
        n--;
        erased_total++;
    }
    real[len] = '\0';
    return 0;
}

/* ---- checks ------------------------------------------------------------- */

static void expect_screen(const char *name, const char *want)
{
    if (strcmp(real, want) != 0) {
        printf("FAIL  %s: screen is \"%s\", wanted \"%s\"\n", name, real, want);
        fails++;
    } else {
        printf("PASS  %s: screen \"%s\"\n", name, real);
    }
}

static void expect_erased(const char *name, size_t want)
{
    if (erased_total != want) {
        printf("FAIL  %s: erased %zu, wanted %zu\n", name, erased_total, want);
        fails++;
    } else {
        printf("PASS  %s: erased %zu\n", name, want);
    }
}

int main(void)
{
    injector *fake = (injector *)0x1;   /* screen.c only passes it through */

    /* A swap touches only the differing tail. */
    stub_reset();
    screen_attach(fake);
    screen_render("meet at 5pm on Monday, uh, no 5pm on tuesday");
    screen_render("meet at 5pm on Tuesday.");
    expect_screen("swap", "meet at 5pm on Tuesday.");
    /* "meet at 5pm on " is kept (15 codepoints); the rest is erased once. */
    expect_erased("swap", strlen("Monday, uh, no 5pm on tuesday"));

    /* A multi-byte prefix is not cut in half. */
    stub_reset();
    screen_attach(fake);
    screen_render("café x");
    screen_render("café y");
    expect_screen("utf8", "café y");
    expect_erased("utf8", 1);           /* only the trailing 'x' */

    /* Whole multi-byte characters are erased whole, never by the byte. */
    stub_reset();
    screen_attach(fake);
    screen_render("café naïve résumé");
    screen_render("café naïve");
    expect_screen("utf8-shrink", "café naïve");

    stub_reset();
    screen_attach(fake);
    screen_render("日本語のテキスト");
    screen_render("日本語");
    expect_screen("cjk", "日本語");

    /* The shared prefix ends inside a character -- "é" and "è" share a first
     * byte -- so the diff must back off to the boundary or leave half a char. */
    stub_reset();
    screen_attach(fake);
    screen_render("café");
    screen_render("cafè");
    expect_screen("mid-char-backoff", "cafè");

    /* A dropped character leaves the erase count matching what landed. The
     * backend cannot type '~'; screen.c must still not over-erase. */
    stub_reset();
    drop = '~';
    screen_attach(fake);
    screen_render("x~y");               /* "xy" lands, 2 codepoints, out of step */
    expect_screen("drop-render", "xy");
    screen_render("z");                 /* resync erases the 2 that landed, not 3 */
    expect_screen("drop-resync", "z");
    expect_erased("drop-resync", 2);

    /* A commit-shaped render arriving while out of step still recovers: it
     * clears what landed and types the final text clean. This is the shape of a
     * cleaned transcript replacing a preview that dropped a character. */
    stub_reset();
    drop = 'q';
    screen_attach(fake);
    screen_render("a quick brown fox");         /* "a uick brown fox" lands */
    drop = 0;
    screen_render("A quick brown fox.");
    screen_release();
    expect_screen("desync-commit", "A quick brown fox.");

    /* After release the text is the user's: the next render erases nothing and
     * appends instead. */
    stub_reset();
    screen_attach(fake);
    screen_render("abc");
    screen_release();
    screen_render("def");
    expect_erased("release", 0);        /* the "abc" is left alone */
    expect_screen("release", "abcdef"); /* new text lands after it */

    screen_detach();

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
