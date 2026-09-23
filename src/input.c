#include "input.h"

void input_update(Input *in)
{
    bool down[BTN_COUNT];
    for (int i = 0; i < BTN_COUNT; i++) down[i] = in->raw[i];
    plat_input_poll(down);
    for (int i = 0; i < BTN_COUNT; i++) {
        int *s = &in->state[i];
        if (down[i]) { if (*s == 2) *s = 0; else if (*s != 0) *s = 2; }
        else         { if (*s == 3) *s = 1; else if (*s != 1) *s = 3; }
    }
}
