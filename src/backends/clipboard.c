/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 AJ Khullar
 *
 * whisprd -- hold-to-talk voice transcription for Linux.
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation. It is distributed WITHOUT ANY WARRANTY;
 * see the LICENSE file or <https://www.gnu.org/licenses/> for details.
 */
/* Universal fallback: put the transcript on the clipboard, then emit a paste
 * chord through uinput. The clipboard carries real UTF-8 so the active layout
 * is irrelevant to the text, and a control chord is the one thing uinput emits
 * reliably on every compositor, GNOME and KDE included. */

#include "../injector.h"
#include "../log.h"
#include "../uinput_kbd.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const config *cfg;
    bool          wayland;
} clip_ctx;

static bool clip_probe(void)
{
    /* uinput is the hard requirement; the copy helper is checked at init. */
    return true;
}

/* Runs `argv` with `text` on its stdin. wl-copy backgrounds itself to serve
 * the selection, so this returns promptly. */
static int run_copy(char *const argv[], const char *text)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        log_err("pipe: %s\n", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_err("fork: %s\n", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[0]);
    size_t len = strlen(text), off = 0;
    while (off < len) {
        ssize_t w = write(pipefd[1], text + off, len - off);
        if (w <= 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)w;
    }
    close(pipefd[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        log_err("%s not found; install wl-clipboard (Wayland) or xclip (X11)\n",
                argv[0]);
        return -1;
    }
    return 0;
}

static void *clip_init(const config *cfg)
{
    clip_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->cfg = cfg;

    const char *wl = getenv("WAYLAND_DISPLAY");
    ctx->wayland = wl && *wl;

    if (uinput_kbd_open() < 0) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

static int clip_send(void *vctx, const char *utf8)
{
    clip_ctx *ctx = vctx;

    int rc;
    if (ctx->wayland) {
        char *argv[] = { "wl-copy", "--type", "text/plain;charset=utf-8", NULL };
        rc = run_copy(argv, utf8);
    } else {
        char *argv[] = { "xclip", "-selection", "clipboard", NULL };
        rc = run_copy(argv, utf8);
    }
    if (rc < 0)
        return -1;

    /* Give the selection owner a moment to be ready to serve the paste. */
    nanosleep(&(struct timespec){ .tv_nsec = 60L * 1000 * 1000 }, NULL);

    return uinput_kbd_chord(ctx->cfg->paste_key, ctx->cfg->paste_mods,
                            ctx->cfg->n_paste_mods);
}

static void clip_destroy(void *vctx)
{
    uinput_kbd_close();
    free(vctx);
}

const inject_backend backend_clipboard = {
    .name    = "clipboard",
    .probe   = clip_probe,
    .init    = clip_init,
    .send    = clip_send,
    .destroy = clip_destroy,
};
