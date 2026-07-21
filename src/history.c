/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "history.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SLUG_MAX 40

static bool   enabled;
static char   dir[512];
static char   cur_path[768];
static time_t last_write;

static void default_dir(char *out, size_t n)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%s/scribe/transcriptions", xdg);
    else
        snprintf(out, n, "%s/.local/share/scribe/transcriptions",
                 getenv("HOME") ? getenv("HOME") : ".");
}

/* mkdir -p. 0700 throughout: these files are a verbatim record of everything
 * spoken at the machine, so no group or other access at any level. */
static int mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0700) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return (mkdir(tmp, 0700) < 0 && errno != EEXIST) ? -1 : 0;
}

/* First few words of an utterance, as a filename-safe stem. Names the session
 * after what was actually said, which is what makes the list scannable --
 * a wall of identical timestamps is not. */
static void slugify(const char *text, char *out, size_t n)
{
    size_t j = 0;
    bool dash = false;

    for (size_t i = 0; text[i] && j + 1 < n && j < SLUG_MAX; i++) {
        unsigned char c = (unsigned char)text[i];
        if (isalnum(c)) {
            out[j++] = (char)tolower(c);
            dash = false;
        } else if (!dash && j) {
            out[j++] = '-';
            dash = true;
        }
    }
    while (j && out[j - 1] == '-')
        j--;
    out[j] = '\0';

    /* Non-latin speech can slugify to nothing; the timestamp still makes the
     * name unique. */
    if (!j)
        snprintf(out, n, "session");
}

static void open_session(const char *first)
{
    char slug[SLUG_MAX + 8];
    slugify(first, slug, sizeof(slug));

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);

    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    snprintf(cur_path, sizeof(cur_path), "%s/%s-%s.txt", dir, stamp, slug);
}

int history_init(const config *cfg)
{
    enabled = cfg->history;
    cur_path[0] = '\0';
    last_write = 0;

    if (!enabled) {
        log_dbg("history: recording disabled\n");
        return 0;
    }

    if (cfg->history_dir[0])
        snprintf(dir, sizeof(dir), "%s", cfg->history_dir);
    else
        default_dir(dir, sizeof(dir));

    if (mkdir_p(dir) < 0) {
        log_err("history: cannot create %s: %s\n", dir, strerror(errno));
        enabled = false;
        return -1;
    }

    log_info("history: recording to %s\n", dir);
    return 0;
}

void history_append(const char *text)
{
    if (!enabled || !text || !*text)
        return;

    time_t now = time(NULL);
    if (!cur_path[0] || now - last_write > HISTORY_GAP_S)
        open_session(text);

    /* Created 0600 rather than created-then-chmodded: between an fopen() at the
     * umask default and a chmod() afterwards there is a window where a verbatim
     * record of everything spoken at this machine is world-readable. O_APPEND
     * because the mode argument only applies when the file is new. */
    int fd = open(cur_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "a");
    if (!f) {
        /* Deliberately does not log the transcript itself. */
        log_warn("history: cannot write %s: %s\n", cur_path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }
    fprintf(f, "%s\n", text);
    fclose(f);

    last_write = now;
}

void history_shutdown(void)
{
    cur_path[0] = '\0';
    last_write = 0;
    enabled = false;
}
