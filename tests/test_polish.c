/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* polish_accept is where the judgement lives: does the model's reply read like
 * a cleaned transcript, or like the model wandering off? No server needed. */
#include "polish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

/* polish.c's log_dbg refers to it. */
int log_verbose;

static void expect(const char *name, const char *raw, const char *cand,
                   const char *want)
{
    char *got = polish_accept(raw, cand);
    int ok = (!want && !got) || (want && got && strcmp(want, got) == 0);
    printf("%s  %-16s -> %s\n", ok ? "PASS" : "FAIL", name,
           got ? got : "(rejected)");
    if (!ok) {
        printf("      wanted: %s\n", want ? want : "(rejected)");
        fails++;
    }
    free(got);
}

int main(void)
{
    /* The headline case: a spoken self-correction, cleaned. */
    expect("self-correct",
           "let's meet at 5pm on Monday, uh, no 5pm on tuesday",
           "Let's meet at 5pm on Tuesday.",
           "Let's meet at 5pm on Tuesday.");

    /* A quote pair the raw did not have is stripped. */
    expect("unwrap-quotes", "hello there", "\"Hello there.\"", "Hello there.");

    /* Trailing whitespace trimmed. */
    expect("trim", "hi there", "Hi there.  \n", "Hi there.");

    /* A chatty preamble adds a line the raw never had: rejected. */
    expect("preamble", "fix this sentence please",
           "Sure! Here is the cleaned version:\n\nFix this sentence, please.",
           NULL);

    /* The model answered the question instead of cleaning it: almost no words
     * survive, so it is rejected. */
    expect("answered", "what time works for you",
           "How about 3 PM on Tuesday?", NULL);

    /* Far longer than the input: the model is explaining itself. */
    expect("too-long", "okay",
           "Okay, and by the way here is a much longer explanation of things.",
           NULL);

    /* Nothing usable. */
    expect("empty", "something", "   ", NULL);
    expect("null", "something", NULL, NULL);

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
