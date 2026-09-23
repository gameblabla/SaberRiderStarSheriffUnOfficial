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
