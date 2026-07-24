/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* What is on screen is tracked two ways, because one of them is not enough.
 * `shown` is the text we believe we typed, and drives the diff that keeps the
 * retraction short. `typed` is how many codepoints actually landed, counted
 * from what the backend reports. They disagree when a backend drops characters
 * it cannot produce -- the uinput layout does this for curly quotes and em
 * dashes -- and when they disagree the count is the one to trust, because it is
 * measured rather than assumed. Erasing by the wrong number does not leave a
 * mess on screen; it eats the user's own text to the left of the caret. */
#include "screen.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

static injector *inj;
static char     *shown;                     /* what we think we typed */
static size_t    typed;                     /* codepoints that really landed */
static bool      synced;                    /* do the two above agree? */

/* Bytes the two share, backed off to a codepoint boundary so a retraction can
 * never cut a multi-byte character in half. */
static size_t common_prefix(const char *a, const char *b)
{
    size_t i = 0;
    while (a[i] && a[i] == b[i])
        i++;
    /* Walk back off any continuation bytes: the first differing byte may be the
     * middle of a character whose earlier bytes matched. */
    while (i > 0 && ((unsigned char)a[i] & 0xC0) == 0x80)
        i--;
    return i;
}

/* Erases everything we put on screen and forgets what it was. Used when the
 * two records disagree: the count is still right, the text is not, so the only
 * honest move is to clear the ground and start again. */
static void resync(void)
{
    if (typed > 0 && injector_erase(inj, typed) < 0)
        log_warn("screen: could not clear the text; leaving it alone\n");
    typed = 0;
    free(shown);
    shown = strdup("");
    synced = true;
}

void screen_attach(injector *injector_in)
{
    inj = injector_in;
    free(shown);
    shown = strdup("");
    typed = 0;
    synced = true;
}

void screen_render(const char *want)
{
    if (!synced)
        resync();

    size_t keep = common_prefix(shown, want);

    size_t drop = injector_utf8_len(shown + keep);
    if (drop > 0) {
        if (injector_erase(inj, drop) < 0) {
            synced = false;
            return;
        }
        typed -= drop;
    }

    const char *tail = want + keep;
    if (*tail) {
        size_t landed = 0;
        int rc = injector_send(inj, tail, &landed);
        typed += landed;
        if (rc < 0 || landed != injector_utf8_len(tail)) {
            /* Either the send failed part-way or the backend could not produce
             * some characters. Both leave us unable to say what is on screen,
             * so mark it and let the next render clear up. */
            synced = false;
            log_dbg("screen: typed %zu of %zu, out of step\n",
                    landed, injector_utf8_len(tail));
            return;
        }
    }

    char *copy = strdup(want);
    if (copy) {
        free(shown);
        shown = copy;
    } else {
        synced = false;
    }
}

void screen_release(void)
{
    free(shown);
    shown = strdup("");
    typed = 0;
    synced = true;
}

size_t screen_owned(void)
{
    return typed;
}

void screen_detach(void)
{
    if (typed > 0)
        injector_erase(inj, typed);
    free(shown);
    shown = NULL;
    typed = 0;
    synced = true;
    inj = NULL;
}
