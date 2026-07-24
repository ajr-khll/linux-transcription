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

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static size_t utf8_encode(unsigned cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* `*p` points at the opening quote. On success returns the decoded string and
 * leaves `*p` just past the closing quote. */
static char *parse_string(const char **p)
{
    const char *s = *p;
    if (*s != '"')
        return NULL;
    s++;

    size_t cap = 64, len = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;

    while (*s && *s != '"') {
        /* Worst case one input step yields 4 bytes, plus the NUL. */
        if (len + 5 > cap) {
            cap *= 2;
            char *q = realloc(out, cap);
            if (!q) {
                free(out);
                return NULL;
            }
            out = q;
        }

        if (*s != '\\') {
            out[len++] = *s++;
            continue;
        }

        s++;
        switch (*s) {
        case '"':  out[len++] = '"';  s++; break;
        case '\\': out[len++] = '\\'; s++; break;
        case '/':  out[len++] = '/';  s++; break;
        case 'b':  out[len++] = '\b'; s++; break;
        case 'f':  out[len++] = '\f'; s++; break;
        case 'n':  out[len++] = '\n'; s++; break;
        case 'r':  out[len++] = '\r'; s++; break;
        case 't':  out[len++] = '\t'; s++; break;
        case 'u': {
            unsigned cp;
            if (hex4(s + 1, &cp) < 0) {
                free(out);
                return NULL;
            }
            s += 5;
            /* Recombine a surrogate pair into one codepoint. */
            if (cp >= 0xD800 && cp <= 0xDBFF && s[0] == '\\' && s[1] == 'u') {
                unsigned lo;
                if (hex4(s + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s += 6;
                }
            }
            len += utf8_encode(cp, out + len);
            break;
        }
        default:
            free(out);
            return NULL;
        }
    }

    if (*s != '"') {
        free(out);
        return NULL;
    }
    out[len] = '\0';
    *p = s + 1;
    return out;
}

char *json_escape_string(const char *s)
{
    size_t cap = 64, len = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        /* Worst case one input byte yields six (\uXXXX), plus the NUL. */
        if (len + 7 > cap) {
            cap *= 2;
            char *q = realloc(out, cap);
            if (!q) {
                free(out);
                return NULL;
            }
            out = q;
        }

        switch (*p) {
        case '"':  out[len++] = '\\'; out[len++] = '"';  break;
        case '\\': out[len++] = '\\'; out[len++] = '\\'; break;
        case '\b': out[len++] = '\\'; out[len++] = 'b';  break;
        case '\f': out[len++] = '\\'; out[len++] = 'f';  break;
        case '\n': out[len++] = '\\'; out[len++] = 'n';  break;
        case '\r': out[len++] = '\\'; out[len++] = 'r';  break;
        case '\t': out[len++] = '\\'; out[len++] = 't';  break;
        default:
            if (*p < 0x20) {
                /* Any other control byte, spelled out. sprintf writes its own
                 * NUL after the six characters, which the next iteration or the
                 * final terminator overwrites. */
                static const char hex[] = "0123456789abcdef";
                out[len++] = '\\';
                out[len++] = 'u';
                out[len++] = '0';
                out[len++] = '0';
                out[len++] = hex[*p >> 4];
                out[len++] = hex[*p & 0xF];
            } else {
                /* Printable ASCII and every UTF-8 continuation byte alike. */
                out[len++] = (char)*p;
            }
        }
    }

    out[len] = '\0';
    return out;
}

char *json_extract_string(const char *json, const char *key)
{
    const char *p = json;
    while (*p) {
        if (*p != '"') {
            p++;
            continue;
        }

        const char *start = p;
        char *tok = parse_string(&p);
        if (!tok) {
            p = start + 1;      /* malformed; resync past the quote */
            continue;
        }

        /* A string followed by ':' was a key, not a value. */
        const char *q = p;
        while (*q && isspace((unsigned char)*q))
            q++;
        if (*q == ':' && strcmp(tok, key) == 0) {
            free(tok);
            q++;
            while (*q && isspace((unsigned char)*q))
                q++;
            return parse_string(&q);
        }
        free(tok);
    }
    return NULL;
}
