#ifndef WHISPRD_INPUT_H
#define WHISPRD_INPUT_H

#include <stdbool.h>

#include "config.h"

typedef void (*hold_cb)(bool holding, void *user);

int  input_init(const config *cfg, hold_cb cb, void *user);
int  input_run(void);       /* blocks in epoll until input_stop() */
void input_stop(void);      /* async-signal-safe */
void input_shutdown(void);

#endif
