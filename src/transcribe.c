/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "transcribe.h"
#include "audio.h"
#include "json_text.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

static const config *conf;
static CURL         *curl;
static char          url[640];
static struct curl_slist *headers;

#define WAV_HEADER_BYTES 44

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

/* Builds a 44-byte RIFF header followed by the PCM, all in memory. */
static uint8_t *wav_wrap(const int16_t *samples, size_t n_samples, size_t *out_len)
{
    uint32_t data_bytes = (uint32_t)(n_samples * sizeof(int16_t));
    size_t total = WAV_HEADER_BYTES + data_bytes;

    uint8_t *w = malloc(total);
    if (!w)
        return NULL;

    memcpy(w + 0, "RIFF", 4);
    put_u32(w + 4, (uint32_t)(total - 8));
    memcpy(w + 8, "WAVE", 4);

    memcpy(w + 12, "fmt ", 4);
    put_u32(w + 16, 16);                    /* PCM fmt chunk size */
    put_u16(w + 20, 1);                     /* format: PCM */
    put_u16(w + 22, 1);                     /* channels */
    put_u32(w + 24, AUDIO_RATE);
    put_u32(w + 28, AUDIO_RATE * 2);        /* byte rate */
    put_u16(w + 32, 2);                     /* block align */
    put_u16(w + 34, 16);                    /* bits per sample */

    memcpy(w + 36, "data", 4);
    put_u32(w + 40, data_bytes);
    memcpy(w + WAV_HEADER_BYTES, samples, data_bytes);

    *out_len = total;
    return w;
}

struct response {
    char  *data;
    size_t len;
};

static size_t on_data(void *ptr, size_t size, size_t nmemb, void *user)
{
    struct response *r = user;
    size_t n = size * nmemb;
    char *p = realloc(r->data, r->len + n + 1);
    if (!p)
        return 0;
    r->data = p;
    memcpy(r->data + r->len, ptr, n);
    r->len += n;
    r->data[r->len] = '\0';
    return n;
}

int transcribe_init(const config *cfg)
{
    conf = cfg;
    curl_global_init(CURL_GLOBAL_DEFAULT);

    curl = curl_easy_init();
    if (!curl) {
        log_err("curl_easy_init failed\n");
        return -1;
    }

    snprintf(url, sizeof(url), "%s/audio/transcriptions", cfg->endpoint_url);

    /* Refused here rather than at the first utterance. Without this the daemon
     * starts, looks healthy, and only fails once the user has already spoken --
     * as a 401 buried in the journal, long after they stopped watching. */
    if (!cfg->api_key[0]) {
        log_err("no API key. Set one of:\n"
                "  api_key = sk-...   in ~/.config/whisprd/config.ini\n"
                "  export OPENAI_API_KEY=sk-...   (takes precedence)\n"
                "Get a key at https://platform.openai.com/api-keys\n");
        return -1;
    }

    char auth[320];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", cfg->api_key);
    headers = curl_slist_append(headers, auth);
    /* libcurl otherwise waits for a 100-continue that some servers never send. */
    headers = curl_slist_append(headers, "Expect:");

    log_info("endpoint %s (model %s)\n", url, cfg->model);
    return 0;
}

char *transcribe_pcm(const int16_t *samples, size_t n_samples)
{
    size_t wav_len;
    uint8_t *wav = wav_wrap(samples, n_samples, &wav_len);
    if (!wav)
        return NULL;

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filename(part, "audio.wav");
    curl_mime_type(part, "audio/wav");
    curl_mime_data(part, (const char *)wav, wav_len);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "model");
    curl_mime_data(part, conf->model, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "response_format");
    curl_mime_data(part, "json", CURL_ZERO_TERMINATED);

    struct response resp = { 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "whisprd/" SCRIBE_VERSION);

    char *text = NULL;
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        log_err("request failed: %s\n", curl_easy_strerror(rc));
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status < 200 || status >= 300) {
            log_err("endpoint returned HTTP %ld: %s\n", status,
                    resp.data ? resp.data : "(empty body)");
        } else if (!resp.data) {
            log_err("empty response body\n");
        } else {
            text = json_extract_string(resp.data, "text");
            if (!text)
                log_err("no \"text\" field in response: %s\n", resp.data);
        }
    }

    free(resp.data);
    curl_mime_free(mime);
    free(wav);
    return text;
}

void transcribe_shutdown(void)
{
    if (curl)
        curl_easy_cleanup(curl);
    curl = NULL;
    if (headers)
        curl_slist_free_all(headers);
    headers = NULL;
    curl_global_cleanup();
}
