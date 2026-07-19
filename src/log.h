/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef WHISPRD_LOG_H
#define WHISPRD_LOG_H

#include <stdio.h>

extern int log_verbose;

#define log_err(...)  fprintf(stderr, "whisprd: error: " __VA_ARGS__)
#define log_warn(...) fprintf(stderr, "whisprd: warn: " __VA_ARGS__)
#define log_info(...) fprintf(stderr, "whisprd: " __VA_ARGS__)
#define log_dbg(...)  do { if (log_verbose) fprintf(stderr, "whisprd: dbg: " __VA_ARGS__); } while (0)

#endif
