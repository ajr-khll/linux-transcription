#ifndef WHISPRD_JSON_TEXT_H
#define WHISPRD_JSON_TEXT_H

/* Purpose-built extractor for the one field we care about in an
 * OpenAI-compatible transcription response: {"text": "..."}.
 *
 * Returns the first string value whose key matches `key`, malloc'd and with
 * JSON escapes (including \uXXXX surrogate pairs) decoded to UTF-8, or NULL
 * if absent. Caller frees. */
char *json_extract_string(const char *json, const char *key);

#endif
