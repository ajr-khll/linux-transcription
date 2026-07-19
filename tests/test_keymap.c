/* Pulls in the backend so the static keymap builder can be exercised, then
 * feeds the generated keymap to xkbcommon and reads the characters back out.
 * If the round-trip matches, the compositor will type what we meant. */
#include "backends/wlr_vk.c"

#include <stdio.h>

static int fails;

static void round_trip(const char *text)
{
    size_t n;
    uint32_t *cps = utf8_decode(text, &n);

    uint32_t distinct[MAX_KEYS];
    size_t nd = 0;
    for (size_t i = 0; i < n; i++)
        if (index_of(distinct, nd, cps[i]) == (size_t)-1 && nd < MAX_KEYS)
            distinct[nd++] = cps[i];

    char *km = build_keymap(distinct, nd);
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        ctx, km, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

    if (!keymap) {
        printf("FAIL  keymap did not compile for: %s\n", text);
        fails++;
        goto out;
    }

    /* Type the text through a real xkb state and compare what comes out. */
    struct xkb_state *st = xkb_state_new(keymap);
    char got[1024] = { 0 };
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        size_t k = index_of(distinct, nd, cps[i]);
        xkb_keycode_t kc = (xkb_keycode_t)(k + 9);
        len += (size_t)xkb_state_key_get_utf8(st, kc, got + len, sizeof(got) - len);
    }
    xkb_state_unref(st);

    char want[1024];
    snprintf(want, sizeof(want), "%s", text);
    for (char *w = want; *w; w++)
        if (*w == '\n') *w = '\r';   /* Return keysym reads back as CR */
    int ok = strcmp(got, want) == 0;
    printf("%s  %zu distinct keys | %s\n", ok ? "PASS" : "FAIL", nd, text);
    if (!ok) {
        printf("      got back: %s\n", got);
        fails++;
    }
    xkb_keymap_unref(keymap);
out:
    xkb_context_unref(ctx);
    free(km);
    free(cps);
}

int main(void)
{
    round_trip("Hello, world!");
    round_trip("The quick brown fox jumps over the lazy dog. 0123456789");
    round_trip("punctuation: \"quotes\" 'apostrophe' #$%&*()[]{}<>@~`^|\\/+=-_");
    round_trip("accents: café naïve résumé über straße àçñõ");
    round_trip("em dash — ellipsis… curly “quotes”");
    round_trip("emoji 😀 and CJK 日本語");
    round_trip("tab\there\nnewline");

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
