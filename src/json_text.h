/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#ifndef SCRIBE_JSON_TEXT_H
#define SCRIBE_JSON_TEXT_H

/* Purpose-built extractor for the one field we care about in an
 * OpenAI-compatible transcription response: {"text": "..."}.
 *
 * Returns the first string value whose key matches `key`, malloc'd and with
 * JSON escapes (including \uXXXX surrogate pairs) decoded to UTF-8, or NULL
 * if absent. Caller frees. */
char *json_extract_string(const char *json, const char *key);

/* Escapes `s` for use as a JSON string value, without the surrounding quotes.
 * The control characters get their short escapes (\", \\, \n, \r, \t, \b, \f);
 * any other byte below 0x20 becomes \uXXXX. Bytes at or above 0x20, including
 * every byte of a UTF-8 sequence, pass through untouched. Returns a malloc'd
 * string the caller frees, or NULL on allocation failure.
 *
 * The inverse of parse_string() in json_text.c, so a string escaped here and
 * decoded there comes back unchanged. */
char *json_escape_string(const char *s);

#endif
