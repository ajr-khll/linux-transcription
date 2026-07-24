/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Watches config.ini and calls back when it changes, so an edit takes effect on
 * its own rather than waiting for a SIGHUP the user has to remember to send. */
#ifndef SCRIBE_CONFWATCH_H
#define SCRIBE_CONFWATCH_H

/* `on_change` runs on the watcher's own thread, so it must do only what a
 * signal handler could: set a flag and wake the main loop. Returns 0 when the
 * watch is up. A failure is reported and is not fatal -- the daemon carries on,
 * and SIGHUP still works. */
int  confwatch_init(const char *path, void (*on_change)(void));

void confwatch_shutdown(void);

#endif
