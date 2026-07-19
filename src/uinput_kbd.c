/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "uinput_kbd.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/uinput.h>

static int fd = -1;

static int emit(int type, int code, int value)
{
    struct input_event ev = { .type = (unsigned short)type,
                              .code = (unsigned short)code,
                              .value = value };
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        log_err("uinput write: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int syn(void)
{
    return emit(EV_SYN, SYN_REPORT, 0);
}

int uinput_kbd_open(void)
{
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        log_err("open /dev/uinput: %s (is the udev rule installed and are you "
                "in the 'input' group?)\n", strerror(errno));
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0)
        goto fail;
    for (int code = KEY_ESC; code < KEY_MAX; code++)
        ioctl(fd, UI_SET_KEYBIT, code);

    struct uinput_setup setup = {
        .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1209, .product = 0x0001, .version = 1 },
    };
    snprintf(setup.name, sizeof(setup.name), "%s", WHISPRD_UINPUT_NAME);

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0)
        goto fail;

    /* The compositor needs a moment to notice the new device; without this the
     * first chord after startup is silently dropped. Paid once, at init. */
    nanosleep(&(struct timespec){ .tv_nsec = 250L * 1000 * 1000 }, NULL);

    log_dbg("uinput keyboard '%s' created\n", WHISPRD_UINPUT_NAME);
    return 0;

fail:
    log_err("uinput setup failed: %s\n", strerror(errno));
    close(fd);
    fd = -1;
    return -1;
}

int uinput_kbd_chord(int key, const int *mods, size_t n_mods)
{
    if (fd < 0)
        return -1;

    for (size_t i = 0; i < n_mods; i++)
        if (emit(EV_KEY, mods[i], 1) < 0)
            return -1;
    if (n_mods && syn() < 0)
        return -1;

    if (emit(EV_KEY, key, 1) < 0 || syn() < 0)
        return -1;
    if (emit(EV_KEY, key, 0) < 0 || syn() < 0)
        return -1;

    for (size_t i = n_mods; i > 0; i--)
        if (emit(EV_KEY, mods[i - 1], 0) < 0)
            return -1;
    if (n_mods && syn() < 0)
        return -1;

    return 0;
}

void uinput_kbd_close(void)
{
    if (fd < 0)
        return;
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    fd = -1;
}
