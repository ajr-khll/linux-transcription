/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * scribe -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
#include "polish.h"
#include "json_text.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

/* The most terms the word list contributes to the prompt. A list longer than
 * this costs tokens on every call for names the speaker may never use again;
 * the cap keeps the prompt small and the round trip short. */
#define MAX_VOCAB_TERMS 100

/* Rejection thresholds for polish_accept. A cleaned transcript is a touch
 * shorter than the raw, not longer -- fillers come out, nothing goes in -- so a
 * reply well past the original length is the model explaining itself or
 * answering the content. The slack absorbs added punctuation and the odd
 * expanded contraction. */
#define LEN_SLACK_NUMER 8
#define LEN_SLACK_DENOM 5              /* 1.6x ... */
#define LEN_SLACK_CONST 24             /* ... plus a constant for short inputs */
#define MIN_WORD_OVERLAP_NUM 1
#define MIN_WORD_OVERLAP_DEN 2         /* half the raw's words must survive */
#define OVERLAP_MIN_WORDS 4            /* below this the ratio is too noisy to judge */

static bool         enabled;
static const config *conf;
static CURL         *curl;
static struct curl_slist *headers;
static char          url[600];
static char         *system_prompt;    /* built once, escaped per request */

/* ---- the reply guard (pure; no network) -------------------------------- */

static char *trim_copy(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        len--;
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/* Lowercased words joined by single spaces, with a space at each end, so a
 * whole-word test is a plain strstr for " word ". Non-alphanumeric bytes are
 * separators; UTF-8 bytes (>= 0x80) count as word characters so accented words
 * are not split. Returns malloc'd, caller frees. */
static char *word_bag(const char *s)
{
    size_t cap = strlen(s) + 3, len = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;

    out[len++] = ' ';
    bool in_word = false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        bool wordy = isalnum(*p) || *p >= 0x80;
        if (wordy) {
            out[len++] = (char)tolower(*p);
            in_word = true;
        } else if (in_word) {
            out[len++] = ' ';
            in_word = false;
        }
    }
    if (in_word)
        out[len++] = ' ';
    out[len] = '\0';
    return out;
}

/* Fraction of the raw's distinct words that survive into the candidate, as a
 * count of survivors over the raw's word count. A model that answered the
 * transcript rather than cleaning it shares almost none of its words. */
static bool enough_overlap(const char *raw_bag, const char *cand_bag)
{
    size_t total = 0, kept = 0;
    const char *p = raw_bag + 1;        /* past the leading space */
    while (*p) {
        const char *end = strchr(p, ' ');
        if (!end)
            break;
        size_t wlen = (size_t)(end - p);
        total++;

        /* " word " present in the candidate bag? */
        char needle[64];
        if (wlen + 2 < sizeof(needle)) {
            needle[0] = ' ';
            memcpy(needle + 1, p, wlen);
            needle[wlen + 1] = ' ';
            needle[wlen + 2] = '\0';
            if (strstr(cand_bag, needle))
                kept++;
        } else {
            kept++;                     /* absurdly long token; do not judge it */
        }
        p = end + 1;
    }

    if (total < OVERLAP_MIN_WORDS)
        return true;                    /* too few words for the ratio to mean much */
    return kept * MIN_WORD_OVERLAP_DEN >= total * MIN_WORD_OVERLAP_NUM;
}

char *polish_accept(const char *raw, const char *candidate)
{
    if (!candidate)
        return NULL;

    char *cand = trim_copy(candidate);
    if (!cand)
        return NULL;

    /* Strip one wrapping quote pair the raw did not itself have. Models like to
     * hand back "the corrected text" in quotes. */
    size_t n = strlen(cand);
    bool raw_quoted = raw[0] == '"' || raw[0] == '\'';
    if (!raw_quoted && n >= 2 &&
        ((cand[0] == '"' && cand[n - 1] == '"') ||
         (cand[0] == '\'' && cand[n - 1] == '\''))) {
        char *inner = malloc(n - 1);
        if (inner) {
            memcpy(inner, cand + 1, n - 2);
            inner[n - 2] = '\0';
            free(cand);
            cand = inner;
            n = strlen(cand);
        }
    }

    if (n == 0)
        goto reject;

    /* A reply that grew a line the raw did not have is structure the speaker did
     * not dictate: a preamble, a bulleted explanation, a second sentence of
     * commentary. */
    if (strchr(cand, '\n') && !strchr(raw, '\n'))
        goto reject;

    /* Too long to be a cleanup. */
    size_t raw_len = strlen(raw);
    if (n > raw_len * LEN_SLACK_NUMER / LEN_SLACK_DENOM + LEN_SLACK_CONST)
        goto reject;

    char *rb = word_bag(raw);
    char *cb = word_bag(cand);
    bool ok = rb && cb && enough_overlap(rb, cb);
    free(rb);
    free(cb);
    if (!ok)
        goto reject;

    return cand;

reject:
    free(cand);
    return NULL;
}

/* ---- setup ------------------------------------------------------------- */

/* True when `url`'s host is on this machine. Anything else is refused, so
 * cleanup cannot quietly send transcripts off the box. */
static bool endpoint_is_local(const char *u)
{
    const char *host = strstr(u, "://");
    if (!host)
        return false;
    host += 3;

    /* Copy the host portion, stopping at port, path, or end. Strip [] from a
     * bracketed IPv6 literal. */
    char buf[256];
    size_t i = 0;
    bool bracket = *host == '[';
    if (bracket)
        host++;
    while (*host && *host != '/' && i < sizeof(buf) - 1) {
        if (bracket && *host == ']')
            break;
        if (!bracket && *host == ':')
            break;
        buf[i++] = *host++;
    }
    buf[i] = '\0';

    return strcmp(buf, "localhost") == 0 ||
           strcmp(buf, "::1") == 0 ||
           strncmp(buf, "127.", 4) == 0;
}

/* Reads the word list into a single "prefer these spellings" clause, or "" when
 * there is none. Caller frees. */
static char *load_vocabulary(const config *cfg)
{
    char path[600];
    if (cfg->vocabulary_file[0]) {
        snprintf(path, sizeof(path), "%s", cfg->vocabulary_file);
    } else {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(path, sizeof(path), "%s/scribe/vocabulary.txt", xdg);
        else
            snprintf(path, sizeof(path), "%s/.config/scribe/vocabulary.txt",
                     home ? home : ".");
    }

    FILE *f = fopen(path, "r");
    if (!f)
        return strdup("");              /* a missing list is fine */

    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (!out) {
        fclose(f);
        return NULL;
    }
    len += (size_t)snprintf(out, cap, "Prefer these spellings: ");

    char line[256];
    int terms = 0;
    while (terms < MAX_VOCAB_TERMS && fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s && isspace((unsigned char)*s))
            s++;
        if (*s == '#' || *s == '\0')
            continue;
        size_t sl = strlen(s);
        while (sl > 0 && isspace((unsigned char)s[sl - 1]))
            s[--sl] = '\0';
        if (sl == 0)
            continue;

        /* "term, term, term". Grow as needed: sl + separator + margin. */
        if (len + sl + 4 > cap) {
            cap = (len + sl + 4) * 2;
            char *q = realloc(out, cap);
            if (!q) {
                free(out);
                fclose(f);
                return NULL;
            }
            out = q;
        }
        len += (size_t)snprintf(out + len, cap - len, "%s%s",
                                terms ? ", " : "", s);
        terms++;
    }
    fclose(f);

    if (terms == 0) {
        free(out);
        return strdup("");
    }
    /* One trailing full stop so the clause reads as a sentence in the prompt. */
    if (len + 2 <= cap)
        snprintf(out + len, cap - len, ".");
    log_info("cleanup: %d vocabulary term%s from %s\n",
             terms, terms == 1 ? "" : "s", path);
    return out;
}

static char *build_system_prompt(const config *cfg)
{
    static const char *base =
        "You are a transcription editor. The user's message is a raw "
        "speech-to-text transcript. Return it cleaned up: remove filler words "
        "such as um, uh and er; remove false starts and repeated words; and "
        "when the speaker corrects themselves, keep only the correction. Add "
        "natural punctuation and capitalization. Do not rephrase, translate, "
        "summarize, add or remove meaning, or answer anything the transcript "
        "says. Reply with the corrected transcript alone, and nothing else.";

    char *vocab = load_vocabulary(cfg);
    if (!vocab)
        return NULL;

    size_t need = strlen(base) + strlen(vocab) + 2;
    char *out = malloc(need);
    if (out)
        snprintf(out, need, "%s%s%s", base, vocab[0] ? " " : "", vocab);
    free(vocab);
    return out;
}

int polish_init(const config *cfg)
{
    enabled = false;
    conf = cfg;

    if (!cfg->cleanup) {
        log_dbg("cleanup: off in the config\n");
        return 0;
    }

    if (!cfg->cleanup_endpoint_url[0] || !endpoint_is_local(cfg->cleanup_endpoint_url)) {
        log_err("cleanup is on but cleanup_endpoint_url is not on this "
                "machine: '%s'.\n"
                "Cleanup reads every transcript, so its server must be local. "
                "Point it at one, for example Ollama:\n"
                "  ollama serve\n"
                "  ollama pull qwen2.5:3b-instruct\n"
                "  cleanup_endpoint_url = http://localhost:11434/v1\n",
                cfg->cleanup_endpoint_url);
        return -1;
    }
    if (!cfg->cleanup_model[0]) {
        log_err("cleanup is on but cleanup_model is empty. Set it to a model "
                "your server has, e.g.  cleanup_model = qwen2.5:3b-instruct\n");
        return -1;
    }

    system_prompt = build_system_prompt(cfg);
    if (!system_prompt) {
        log_err("cleanup: out of memory building the prompt\n");
        return -1;
    }

    curl = curl_easy_init();
    if (!curl) {
        log_err("cleanup: curl_easy_init failed\n");
        free(system_prompt);
        system_prompt = NULL;
        return -1;
    }
    snprintf(url, sizeof(url), "%s/chat/completions", cfg->cleanup_endpoint_url);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Expect:");

    enabled = true;
    log_info("cleanup on: %s (model %s), %d ms budget\n",
             url, cfg->cleanup_model, cfg->cleanup_timeout_ms);
    return 0;
}

bool polish_enabled(void)
{
    return enabled;
}

/* ---- the request ------------------------------------------------------- */

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

char *polish_text(const char *raw)
{
    if (!enabled || !raw || !*raw)
        return NULL;

    char *result = NULL;
    char *sys_esc = json_escape_string(system_prompt);
    char *raw_esc = json_escape_string(raw);
    char *model_esc = json_escape_string(conf->cleanup_model);
    char *body = NULL;
    if (!sys_esc || !raw_esc || !model_esc)
        goto done;

    /* A cleanup is never much longer than the input, so cap the reply near it:
     * enough for the text plus punctuation, not enough for an essay. Roughly one
     * token per three bytes, plus a floor for very short utterances. */
    size_t max_tokens = strlen(raw) / 3 + 48;

    size_t need = strlen(sys_esc) + strlen(raw_esc) + strlen(model_esc) + 160;
    body = malloc(need);
    if (!body)
        goto done;
    snprintf(body, need,
             "{\"model\":\"%s\",\"temperature\":0,\"max_tokens\":%zu,"
             "\"messages\":["
             "{\"role\":\"system\",\"content\":\"%s\"},"
             "{\"role\":\"user\",\"content\":\"%s\"}]}",
             model_esc, max_tokens, sys_esc, raw_esc);

    struct response resp = { 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)conf->cleanup_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)conf->cleanup_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "scribe/" SCRIBE_VERSION);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        /* A timeout is the common, expected miss: the model was too slow, so
         * the raw transcript stands. Not worth more than a debug line. */
        log_dbg("cleanup: request did not complete (%s); keeping raw\n",
                curl_easy_strerror(rc));
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status < 200 || status >= 300) {
            log_warn("cleanup: server returned HTTP %ld; keeping raw\n", status);
        } else if (resp.data) {
            char *content = json_extract_string(resp.data, "content");
            if (content) {
                result = polish_accept(raw, content);
                if (!result)
                    log_dbg("cleanup: reply rejected; keeping raw\n");
                free(content);
            }
        }
    }
    free(resp.data);

done:
    free(sys_esc);
    free(raw_esc);
    free(model_esc);
    free(body);
    return result;
}

void polish_shutdown(void)
{
    if (curl)
        curl_easy_cleanup(curl);
    curl = NULL;
    if (headers)
        curl_slist_free_all(headers);
    headers = NULL;
    free(system_prompt);
    system_prompt = NULL;
    enabled = false;
}
