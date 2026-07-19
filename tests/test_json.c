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

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
