#include "input.h"

void input_update(Input *in)
{
    bool down[BTN_COUNT];
    for (int i = 0; i < BTN_COUNT; i++) down[i] = in->raw[i];
    plat_input_poll(down);
    plat_input_shoulders(in->shoulder);
    for (int i = 0; i < BTN_COUNT; i++) {
        int *s = &in->state[i];
        if (down[i]) { if (*s == 2) *s = 0; else if (*s != 0) *s = 2; }
        else         { if (*s == 3) *s = 1; else if (*s != 1) *s = 3; }
    }
}

void input_stick(bool down[BTN_COUNT], int x, int y, int range)
{
    int dead = range * 3 / 10;   /* 30%: past the stick's rest wobble on worn pads */
    if (x * x + y * y < dead * dead) return;
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    /* a diagonal once the minor axis passes tan(22.5 deg) ~ 0.414 of the major one (106/256) */
    bool h = ax * 256 >= ay * 106, v = ay * 256 >= ax * 106;
    if (h) down[x < 0 ? BTN_LEFT : BTN_RIGHT] = true;
    if (v) down[y < 0 ? BTN_UP : BTN_DOWN] = true;
}
