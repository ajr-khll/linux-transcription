/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef SCRIBE_INPUT_H
#define SCRIBE_INPUT_H

#include <stdbool.h>

#include "config.h"

typedef void (*hold_cb)(bool holding, void *user);

int  input_init(const config *cfg, hold_cb cb, void *user);
int  input_run(void);       /* blocks in epoll until input_stop() */
void input_stop(void);      /* async-signal-safe */
void input_shutdown(void);

#endif
