/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Just enough WAV to feed the two model smoke tests, which are the only parts
 * of scribe that read audio from a file. Header-only: both tests are single
 * translation units built straight from the source tree. */
#ifndef SCRIBE_TEST_WAV_H
#define SCRIBE_TEST_WAV_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t wav_rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t wav_rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* Returns malloc'd samples from a 16-bit mono PCM WAV, or NULL. */
static int16_t *wav_load(const char *path, size_t *n_out, uint32_t *rate_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }

    unsigned char hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a WAV file\n", path);
        fclose(f);
        return NULL;
    }

    uint16_t channels = 0, bits = 0;
    unsigned char ck[8];
    while (fread(ck, 1, sizeof(ck), f) == sizeof(ck)) {
        uint32_t len = wav_rd32(ck + 4);

        if (!memcmp(ck, "fmt ", 4) && len >= 16) {
            unsigned char fmt[16];
            if (fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt))
                break;
            channels  = wav_rd16(fmt + 2);
            *rate_out = wav_rd32(fmt + 4);
            bits      = wav_rd16(fmt + 14);
            fseek(f, (long)len - 16, SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            if (channels != 1 || bits != 16) {
                fprintf(stderr, "%s: need mono 16-bit PCM, got %u ch / %u bit\n",
                        path, channels, bits);
                break;
            }
            size_t n = len / sizeof(int16_t);
            int16_t *s = malloc(n * sizeof(*s));
            if (!s || fread(s, sizeof(*s), n, f) != n) {
                free(s);
                break;
            }
            fclose(f);
            *n_out = n;
            return s;
        } else {
            fseek(f, (long)len + (len & 1), SEEK_CUR);   /* chunks pad to even */
        }
    }

    fprintf(stderr, "%s: no usable data chunk\n", path);
    fclose(f);
    return NULL;
}

#endif
