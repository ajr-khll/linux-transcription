#ifndef WHISPRD_INJECTOR_H
#define WHISPRD_INJECTOR_H

#include <stdbool.h>

#include "config.h"

typedef struct injector injector;

/* Detects the session and picks a backend, unless cfg->backend forces one. */
injector *injector_init(const config *cfg);
int       injector_send(injector *inj, const char *utf8_text);
void      injector_destroy(injector *inj);

/* Backend vtable. Each backend file exports one of these. */
typedef struct {
    const char *name;
    bool  (*probe)(void);                    /* can this run right now? */
    void *(*init)(const config *cfg);        /* NULL on failure */
    int   (*send)(void *ctx, const char *utf8);
    void  (*destroy)(void *ctx);
} inject_backend;

#ifdef WITH_WLR_VK
extern const inject_backend backend_wlr_vk;
#endif
extern const inject_backend backend_clipboard;

#endif
