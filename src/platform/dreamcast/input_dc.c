/* the first controller on the maple bus: D-pad or stick move, A jump, B / X shoot, L / R triggers aim,
 * Y power attack, Start pause (the PC pad layout: south jump, east / west shoot, north power) */
#include "../../input.h"
#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

void plat_input_poll(bool down[BTN_COUNT])
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return;
    cont_state_t *st = (cont_state_t *)maple_dev_status(dev);
    if (!st) return;
    uint32_t b = st->buttons;
    down[BTN_LEFT]  |= (b & CONT_DPAD_LEFT) || st->joyx < -64;
    down[BTN_RIGHT] |= (b & CONT_DPAD_RIGHT) || st->joyx > 64;
    down[BTN_UP]    |= (b & CONT_DPAD_UP) || st->joyy < -64;
    down[BTN_DOWN]  |= (b & CONT_DPAD_DOWN) || st->joyy > 64;
    down[BTN_JUMP]  |= (b & CONT_A) != 0;
    down[BTN_SHOOT] |= (b & (CONT_B | CONT_X)) != 0;
    down[BTN_AIM]   |= st->ltrig > 64 || st->rtrig > 64;
    down[BTN_PAUSE] |= (b & CONT_START) != 0;
    down[BTN_POWER] |= (b & CONT_Y) != 0;
}

/* A+B+X+Y+Start: back to the BIOS menu (the usual Dreamcast reset) */
bool dc_reset_combo(void)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
    uint32_t all = CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START;
    return st && (st->buttons & all) == all;
}

/* the Dreamcast pad layout is fixed */
bool plat_bind_supported(void) { return false; }
void plat_bind_label(int dev, int btn, int slot, char *buf, size_t n) { if (n) buf[0] = 0; }
void plat_bind_capture(int dev, int btn, int slot) { }
bool plat_bind_capturing(void) { return false; }
void plat_bind_cancel(void) { }
void plat_bind_defaults(int dev) { }
void plat_bind_save(void) { }
