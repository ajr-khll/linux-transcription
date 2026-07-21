/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef SCRIBE_UINPUT_KBD_H
#define SCRIBE_UINPUT_KBD_H

#include <stddef.h>

/* The input thread skips any device with this name, which is what keeps our
 * own injected keystrokes from being read back as hotkey events. */
#define SCRIBE_UINPUT_NAME "whisprd-virtual-keyboard"

int  uinput_kbd_open(void);

/* Presses `mods`, taps `key`, releases `mods`. */
int  uinput_kbd_chord(int key, const int *mods, size_t n_mods);

void uinput_kbd_close(void);

#endif
