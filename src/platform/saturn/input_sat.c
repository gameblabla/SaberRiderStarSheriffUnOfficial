/* the pad in port 1, else port 2 (plan 10): D-pad moves, B jump, C shoot, A power attack (Y / Z / X: the same on the top row),
 * L / R aim, Start pause */
#include "../../input.h"
#include "sat_internal.h"
#include <yaul.h>

/* libyaul: `pressed` is the buttons down now, `held` the ones that went down since the last read */
static smpc_peripheral_digital_t pad;

/* Only peripherals whose first two data bytes are the Saturn pad buttons count: the digital pad, the racing wheel and
 * the 3D pad. A keyboard or mouse in port 1 would otherwise read as random buttons; the pad in port 2 is used then. */
static void read_pad(void)
{
    for (uint8_t port = 1; port <= 2; port++) {
        smpc_peripheral_digital_port(port, &pad);
        if (pad.connected && (pad.type == ID_DIGITAL || pad.type == ID_RACING || pad.type == ID_ANALOG)) return;
    }
    pad.connected = 0;
}

void plat_input_poll(bool down[BTN_COUNT])
{
    read_pad();
    if (!pad.connected) return;
    down[BTN_LEFT]  |= pad.pressed.button.left;
    down[BTN_RIGHT] |= pad.pressed.button.right;
    down[BTN_UP]    |= pad.pressed.button.up;
    down[BTN_DOWN]  |= pad.pressed.button.down;
    down[BTN_JUMP]  |= pad.pressed.button.b || pad.pressed.button.y;
    down[BTN_SHOOT] |= pad.pressed.button.c || pad.pressed.button.z;
    down[BTN_POWER] |= pad.pressed.button.a || pad.pressed.button.x;
    down[BTN_AIM]   |= pad.pressed.button.l || pad.pressed.button.r;
    down[BTN_PAUSE] |= pad.pressed.button.start;
}

/* A+B+C+Start: back to the system menu (the Saturn's usual reset combo) */
bool sat_reset_combo(void)
{
    read_pad();
    return pad.connected && pad.pressed.button.a && pad.pressed.button.b && pad.pressed.button.c && pad.pressed.button.start;
}

/* the pad layout is fixed for now (OPTIONS > CONTROLS hidden) */
bool plat_bind_supported(void) { return false; }
void plat_bind_label(int dev, int btn, int slot, char *buf, size_t n) { (void)dev; (void)btn; (void)slot; if (n) buf[0] = 0; }
void plat_bind_capture(int dev, int btn, int slot) { (void)dev; (void)btn; (void)slot; }
bool plat_bind_capturing(void) { return false; }
void plat_bind_cancel(void) { }
void plat_bind_defaults(int dev) { (void)dev; }
void plat_bind_save(void) { }
