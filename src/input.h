#pragma once
/* Button states as in the original: 0 held, 2 just pressed, 1 up, 3 just released. */
#include <stdbool.h>
enum { BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_JUMP, BTN_SHOOT, BTN_AIM, BTN_PAUSE, BTN_POWER, BTN_COUNT };   /* POWER: ours (the heroes' power attacks) */
typedef struct { int state[BTN_COUNT]; bool raw[BTN_COUNT]; } Input;
/* once per fixed step: raw[] (keyboard / script, set by the platform and the app) OR the pads (plat_input_poll) */
void input_update(Input *in);
/* platform: OR the controllers' current buttons into down[] (platform/<name>/input_*.c) */
void plat_input_poll(bool down[BTN_COUNT]);
static inline bool btn_down(const Input *in, int b) { return (in->state[b] & ~2) == 0; }
static inline bool btn_pressed(const Input *in, int b) { return in->state[b] == 2; }

/* Control remapping (OPTIONS > CONTROLS). Platforms without it return false from plat_bind_supported() and stub the rest.
 * Every button has BIND_SLOTS bindings per device; binding a key / pad button that another slot holds swaps the two. */
#include <stddef.h>
#define BIND_SLOTS 2
enum { BIND_KEYBOARD, BIND_PAD, BIND_DEVICES };
bool plat_bind_supported(void);
void plat_bind_label(int dev, int btn, int slot, char *buf, size_t n);   /* upper case; "-" when unbound */
void plat_bind_capture(int dev, int btn, int slot);   /* the next key / pad button pressed goes there (Esc cancels, Del clears) */
bool plat_bind_capturing(void);
void plat_bind_cancel(void);
void plat_bind_defaults(int dev);
void plat_bind_save(void);
