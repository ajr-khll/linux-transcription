/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Watches the config file so an edit applies by itself.
 *
 * The watch is on the *directory*, not the file, and that is the whole trick.
 * Almost nothing edits a config file in place: scribe-menu writes a temp file
 * and renames it over the target, and vim, emacs and every other careful editor
 * do the same, because a rename is atomic and a half-written config is not. The
 * rename replaces the inode. A watch on the file follows the old inode into the
 * bin, so it fires once, for the very first save, and is silently dead
 * afterwards -- the worst kind of broken, because it demos perfectly.
 *
 * Watching the directory and filtering on the name survives that, and picks up
 * a config file created after the daemon started as a bonus.
 */
#include "confwatch.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

/* A single save can raise several events -- a create, a write, a rename -- and
 * reloading on each would tear the daemon down and stand it back up two or
 * three times per keystroke of the settings panel. Wait for this much quiet
 * before acting. Short enough to feel immediate, long enough to coalesce. */
#define SETTLE_MS 200

static int       ino_fd = -1;
static int       stop_fd = -1;
static pthread_t thread;
static bool      running;

static char watch_name[NAME_MAX + 1];       /* the basename we care about */
static void (*changed_cb)(void);

/* True when this batch of events mentions our file. */
static bool batch_mentions_us(const char *buf, ssize_t len)
{
    bool hit = false;
    for (ssize_t off = 0; off + (ssize_t)sizeof(struct inotify_event) <= len; ) {
        const struct inotify_event *e = (const struct inotify_event *)(buf + off);
        if (e->len > 0 && strcmp(e->name, watch_name) == 0)
            hit = true;
        off += (ssize_t)sizeof(*e) + e->len;
    }
    return hit;
}

static void *watch_thread(void *arg)
{
    (void)arg;

    /* inotify_event carries a flexible name, so the read buffer has to hold the
     * struct plus the longest name the kernel might return. */
    char buf[16 * (sizeof(struct inotify_event) + NAME_MAX + 1)];

    for (;;) {
        struct pollfd pfd[2] = {
            { .fd = ino_fd,  .events = POLLIN },
            { .fd = stop_fd, .events = POLLIN },
        };

        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            log_warn("config watch: poll: %s\n", strerror(errno));
            return NULL;
        }
        if (pfd[1].revents & POLLIN)
            return NULL;                    /* confwatch_shutdown */

        ssize_t n = read(ino_fd, buf, sizeof(buf));
        if (n <= 0)
            continue;

        bool ours = batch_mentions_us(buf, n);

        /* Drain whatever else the same save produces, then act once. */
        for (;;) {
            struct pollfd more[2] = {
                { .fd = ino_fd,  .events = POLLIN },
                { .fd = stop_fd, .events = POLLIN },
            };
            int r = poll(more, 2, SETTLE_MS);
            if (r <= 0)
                break;                      /* quiet: the save is finished */
            if (more[1].revents & POLLIN)
                return NULL;
            n = read(ino_fd, buf, sizeof(buf));
            if (n > 0 && batch_mentions_us(buf, n))
                ours = true;
        }

        if (ours && changed_cb)
            changed_cb();
    }
}

int confwatch_init(const char *path, void (*on_change)(void))
{
    if (running)
        return 0;

    /* Split into directory and name without touching the caller's string.
     * dirname/basename may modify their argument, so both get a copy. */
    char dir_buf[512], name_buf[512];
    snprintf(dir_buf, sizeof(dir_buf), "%s", path);
    snprintf(name_buf, sizeof(name_buf), "%s", path);

    char *slash = strrchr(dir_buf, '/');
    const char *dir, *name;
    if (slash) {
        *slash = '\0';
        dir = dir_buf[0] ? dir_buf : "/";
        name = strrchr(name_buf, '/') + 1;
    } else {
        dir = ".";
        name = name_buf;
    }
    if (!*name) {
        log_warn("config watch: '%s' names no file\n", path);
        return -1;
    }
    /* Rejected rather than truncated. A shortened name would compile a watch
     * that compares against a filename nothing will ever be called, and then
     * sit there quietly never firing. */
    if (strlen(name) >= sizeof(watch_name)) {
        log_warn("config watch: '%s' has too long a filename to watch\n", path);
        return -1;
    }
    snprintf(watch_name, sizeof(watch_name), "%s", name);

    ino_fd = inotify_init1(IN_CLOEXEC);
    if (ino_fd < 0) {
        log_warn("config watch: inotify_init1: %s\n", strerror(errno));
        return -1;
    }

    /* CLOSE_WRITE catches an in-place edit; MOVED_TO catches the rename that
     * every careful editor and scribe-menu actually use; CREATE catches a
     * config file that did not exist when the daemon started. */
    if (inotify_add_watch(ino_fd, dir,
                          IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
        log_warn("config watch: cannot watch %s: %s\n", dir, strerror(errno));
        log_warn("edits will need: systemctl --user reload scribe\n");
        close(ino_fd);
        ino_fd = -1;
        return -1;
    }

    stop_fd = eventfd(0, EFD_CLOEXEC);
    if (stop_fd < 0) {
        log_warn("config watch: eventfd: %s\n", strerror(errno));
        close(ino_fd);
        ino_fd = -1;
        return -1;
    }

    changed_cb = on_change;
    if (pthread_create(&thread, NULL, watch_thread, NULL) != 0) {
        log_warn("config watch: cannot start the watch thread\n");
        close(ino_fd);
        close(stop_fd);
        ino_fd = stop_fd = -1;
        return -1;
    }

    running = true;
    log_dbg("config watch: watching %s in %s\n", watch_name, dir);
    return 0;
}

void confwatch_shutdown(void)
{
    if (!running)
        return;

    uint64_t one = 1;
    ssize_t ignored = write(stop_fd, &one, sizeof(one));
    (void)ignored;

    pthread_join(thread, NULL);

    close(ino_fd);
    close(stop_fd);
    ino_fd = stop_fd = -1;
    changed_cb = NULL;
    running = false;
}
