/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "input.h"
#include "log.h"
#include "uinput_kbd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <libevdev/libevdev.h>

#define MAX_DEVICES 32

static const config *conf;
static hold_cb       callback;
static void         *cb_user;

static struct libevdev *devs[MAX_DEVICES];
static int              dev_fds[MAX_DEVICES];
static size_t           n_devs;

static int epfd = -1;
static int stopfd = -1;

/* Bit per keycode, so a chord's modifiers can be checked on the hotkey edge. */
static unsigned char pressed[(KEY_MAX + 7) / 8];
static bool holding;

static void set_pressed(int code, bool down)
{
    if (code < 0 || code > KEY_MAX)
        return;
    if (down)
        pressed[code / 8] |= (unsigned char)(1u << (code % 8));
    else
        pressed[code / 8] &= (unsigned char)~(1u << (code % 8));
}

static bool is_pressed(int code)
{
    if (code < 0 || code > KEY_MAX)
        return false;
    return (pressed[code / 8] >> (code % 8)) & 1u;
}

/* A real keyboard, in the sense that matters here: it can send the letters we
 * might be asked to watch for. Mice and such advertise EV_KEY too. */
static bool looks_like_keyboard(struct libevdev *dev)
{
    if (!libevdev_has_event_type(dev, EV_KEY))
        return false;
    return libevdev_has_event_code(dev, EV_KEY, KEY_A) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_Z) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_SPACE);
}

static void add_device(const char *path)
{
    if (n_devs >= MAX_DEVICES)
        return;

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno != EACCES)
            log_dbg("open %s: %s\n", path, strerror(errno));
        return;
    }

    struct libevdev *dev = NULL;
    if (libevdev_new_from_fd(fd, &dev) < 0) {
        close(fd);
        return;
    }

    const char *name = libevdev_get_name(dev);

    /* Never listen to ourselves: injected keystrokes read back as input would
     * be a feedback loop. */
    if (name && strcmp(name, SCRIBE_UINPUT_NAME) == 0) {
        log_dbg("skipping own virtual keyboard at %s\n", path);
        goto skip;
    }
    if (!looks_like_keyboard(dev))
        goto skip;

    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = (uint32_t)n_devs };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        log_err("epoll_ctl: %s\n", strerror(errno));
        goto skip;
    }

    log_info("watching keyboard: %s (%s)\n", name ? name : "?", path);
    devs[n_devs] = dev;
    dev_fds[n_devs] = fd;
    n_devs++;
    return;

skip:
    libevdev_free(dev);
    close(fd);
}

static void enumerate(void)
{
    DIR *d = opendir("/dev/input");
    if (!d) {
        log_err("opendir /dev/input: %s\n", strerror(errno));
        return;
    }
    /* Sorted-ish is not required; order does not matter. */
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        char path[288];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        add_device(path);
    }
    closedir(d);
}

static bool mods_satisfied(void)
{
    for (size_t i = 0; i < conf->n_mods; i++)
        if (!is_pressed(conf->mod_codes[i]))
            return false;
    return true;
}

static void handle_key(int code, int value)
{
    if (value == 2)     /* autorepeat: state is unchanged */
        return;

    set_pressed(code, value == 1);

    if (code != conf->hotkey_code)
        return;

    if (value == 1 && !holding && mods_satisfied()) {
        holding = true;
        callback(true, cb_user);
    } else if (value == 0 && holding) {
        holding = false;
        callback(false, cb_user);
    }
}

int input_init(const config *cfg, hold_cb cb, void *user)
{
    conf = cfg;
    callback = cb;
    cb_user = user;

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        log_err("epoll_create1: %s\n", strerror(errno));
        return -1;
    }

    stopfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stopfd < 0) {
        log_err("eventfd: %s\n", strerror(errno));
        return -1;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = UINT32_MAX };
    epoll_ctl(epfd, EPOLL_CTL_ADD, stopfd, &ev);

    enumerate();
    if (n_devs == 0) {
        log_err("no readable keyboards found under /dev/input "
                "(add yourself to the 'input' group?)\n");
        return -1;
    }
    return 0;
}

int input_run(void)
{
    struct epoll_event events[MAX_DEVICES + 1];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_DEVICES + 1, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_err("epoll_wait: %s\n", strerror(errno));
            return -1;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.u32 == UINT32_MAX)
                return 0;               /* input_stop() */

            struct libevdev *dev = devs[events[i].data.u32];
            struct input_event ie;
            int rc;
            while ((rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ie))
                       == LIBEVDEV_READ_STATUS_SUCCESS) {
                if (ie.type == EV_KEY)
                    handle_key(ie.code, ie.value);
            }
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                /* Dropped events: resync and rebuild state from the device. */
                while (libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ie)
                           == LIBEVDEV_READ_STATUS_SYNC) {
                    if (ie.type == EV_KEY)
                        handle_key(ie.code, ie.value);
                }
            }
        }
    }
}

void input_stop(void)
{
    if (stopfd >= 0) {
        uint64_t one = 1;
        ssize_t ignored = write(stopfd, &one, sizeof(one));
        (void)ignored;
    }
}

void input_shutdown(void)
{
    for (size_t i = 0; i < n_devs; i++) {
        libevdev_free(devs[i]);
        close(dev_fds[i]);
    }
    n_devs = 0;
    if (stopfd >= 0)
        close(stopfd);
    if (epfd >= 0)
        close(epfd);
    stopfd = epfd = -1;
}
