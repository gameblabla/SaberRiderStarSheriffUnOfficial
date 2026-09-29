/* platform/input.h for the WASM port: the page owns the bindings.
 *
 * The core's own remapping (OPTIONS > CONTROLS, plat_bind_*) is switched off here - plat_bind_supported returns
 * false, as on the Saturn and the Dreamcast - because the controls panel in the page's sidebar is where a player
 * on the web configures them, and it is a better place for it than a menu drawn inside the canvas: a touch
 * control's size and position can be adjusted there, and the bindings are stored by the page (localStorage)
 * rather than in a file inside a read-only virtual disc.
 *
 * So the input path is: the page resolves the keyboard, the gamepad and the touch overlay into the game's nine
 * buttons and the two shoulders once a frame and calls wasm_input_set, and plat_input_poll only reports that
 * state (plus the analog stick through the core's own input_stick, so the SENSITIVITY option still works). */
#include "../../input.h"
#include "wasm_internal.h"
#include "../../game.h"
#include <string.h>

static uint32_t buttons;             /* a bit per BTN_* */
static bool shoulder_l, shoulder_r;
static int stick_x, stick_y;
static bool stick_active;

void input_set(uint32_t b, bool sh_l, bool sh_r, int sx, int sy, bool active)
{
    buttons = b;
    shoulder_l = sh_l;
    shoulder_r = sh_r;
    stick_x = sx;
    stick_y = sy;
    stick_active = active;
}

void plat_input_poll(bool down[BTN_COUNT])
{
    for (int b = 0; b < BTN_COUNT; b++) down[b] = down[b] || (buttons & (1u << b)) != 0;
    /* the left stick always moves (the PC's rule: plat_input_poll in input_sdl.c). The page sends it in the
     * -32768..32768 range the Gamepad API uses, and the core's own dead zone and 8-way wedges apply. */
    if (stick_active) input_stick(down, stick_x, stick_y, 32768);
}
void plat_input_shoulders(bool sh[2]) { sh[0] = shoulder_l; sh[1] = shoulder_r; }

/* ---- the remapping hooks: none, the page's panel is the place ---- */
bool plat_bind_supported(void) { return false; }
void plat_bind_label(int dev, int btn, int slot, char *buf, size_t n) { (void)dev; (void)btn; (void)slot; if (n) buf[0] = 0; }
void plat_bind_capture(int dev, int btn, int slot) { (void)dev; (void)btn; (void)slot; }
bool plat_bind_capturing(void) { return false; }
void plat_bind_cancel(void) { }
void plat_bind_defaults(int dev) { (void)dev; }
void plat_bind_save(void) { }

/* ---- the debug keys (F1 collision, F2 free camera, the arrows) ----
 * A keyboard-only feature (game.h: DBG_KEY_*), pushed by the page the same way the buttons are. */
static bool dbg[DBG_KEY_COUNT];
void input_debug_key(int key, bool down)
{
    if (key >= 0 && key < DBG_KEY_COUNT) dbg[key] = down;
}
bool wasm_input_debug(int key) { return key >= 0 && key < DBG_KEY_COUNT && dbg[key]; }
