/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "json_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void check(const char *json, const char *want)
{
    char *got = json_extract_string(json, "text");
    int ok = (!want && !got) || (want && got && strcmp(want, got) == 0);
    printf("%s  %-42s -> %s\n", ok ? "PASS" : "FAIL", json, got ? got : "(null)");
    if (!ok) {
        printf("      wanted: %s\n", want ? want : "(null)");
        fails++;
    }
    free(got);
}

/* Escapes `raw`, checks it matches `want`, then decodes it back through
 * json_extract_string and checks it round trips to the original. */
static void check_escape(const char *raw, const char *want)
{
    char *got = json_escape_string(raw);
    int ok = got && strcmp(got, want) == 0;
    printf("%s  escape %-24s -> %s\n", ok ? "PASS" : "FAIL", raw, got ? got : "(null)");
    if (!ok) {
        printf("      wanted: %s\n", want);
        fails++;
    }

    if (got) {
        /* {"text":"<escaped>"} must decode back to raw. */
        char obj[512];
        snprintf(obj, sizeof(obj), "{\"text\":\"%s\"}", got);
        char *back = json_extract_string(obj, "text");
        int round = back && strcmp(back, raw) == 0;
        if (!round) {
            printf("FAIL  round trip %s -> %s\n", raw, back ? back : "(null)");
            fails++;
        }
        free(back);
    }
    free(got);
}

int main(void)
{
    check("{\"text\":\"hello world\"}", "hello world");
    check("{\"text\": \" leading space\" }", " leading space");
    check("{\"task\":\"transcribe\",\"text\":\"real one\"}", "real one");
    /* "text" appearing as a *value* must not be mistaken for the key */
    check("{\"a\":\"text\",\"text\":\"real\"}", "real");
    check("{\"text\":\"caf\\u00e9 \\\"quoted\\\"\\n\"}", "café \"quoted\"\n");
    check("{\"text\":\"\\ud83d\\ude00\"}", "\xf0\x9f\x98\x80");
    check("{\"text\":\"\"}", "");
    check("{\"error\":{\"message\":\"nope\"}}", NULL);
    check("not json at all", NULL);
    check("{\"segments\":[{\"text\":\"nested\"}]}", "nested");

    check_escape("plain", "plain");
    check_escape("say \"hi\"", "say \\\"hi\\\"");
    check_escape("a\\b", "a\\\\b");
    check_escape("line\none\ttab", "line\\none\\ttab");
    check_escape("bell\x07here", "bell\\u0007here");
    check_escape("café", "café");           /* UTF-8 passes through */
    check_escape("", "");

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
