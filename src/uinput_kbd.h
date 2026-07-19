#ifndef WHISPRD_UINPUT_KBD_H
#define WHISPRD_UINPUT_KBD_H

#include <stddef.h>

/* The input thread skips any device with this name, which is what keeps our
 * own injected keystrokes from being read back as hotkey events. */
#define WHISPRD_UINPUT_NAME "whisprd-virtual-keyboard"

int  uinput_kbd_open(void);

/* Presses `mods`, taps `key`, releases `mods`. */
int  uinput_kbd_chord(int key, const int *mods, size_t n_mods);

void uinput_kbd_close(void);

#endif
