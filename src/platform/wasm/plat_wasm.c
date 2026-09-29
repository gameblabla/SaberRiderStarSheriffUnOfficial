/* platform/plat.h for the WASM port: the browser's canvas and window in place of a display mode.
 *
 * The SCREEN option (render.h/plat.h's `screen`: 0 fullscreen, n a window n+1 times the game size) becomes the
 * page's canvas scale, which the page reads back through wasm_screen_choices(), and the RATIO option stays the
 * game's (WIDE 426x240, 4:3 320x240, STRETCH): the framebuffer is always the internal 426x240 and the narrower
 * modes simply leave columns at the edge black, so the canvas is a fixed size and CSS scales it.
 *
 * The SABER_* debug switches come from a table the page fills (the sidebar's developer box, or the URL's query
 * string), which is how a console's saber.env / SABER.ENV works on the other ports: the core asks
 * plat_getenv and never touches the environment itself. */
#include "../plat.h"
#include "wasm_internal.h"
#include <stdio.h>
#include <string.h>

/* ---- the page's clock: performance.now() as a double, pushed in once a frame (main_wasm.c's wasm_frame) ---- */
static double now_ms;
uint64_t set_clock(double ms) { now_ms = ms; return (uint64_t)(ms < 0 ? 0 : ms); }
uint64_t plat_ticks_ms(void) { return (uint64_t)(now_ms < 0 ? 0 : now_ms); }

/* ---- the SABER_* switches ---- */
#define MAX_ENV 48
static struct { char name[32]; char value[512]; } env[MAX_ENV];
static int nenv;

int env_set(const char *name, const char *value)
{
    if (!name || !name[0]) return 0;
    for (int i = 0; i < nenv; i++) if (!strcmp(env[i].name, name)) {
        snprintf(env[i].value, sizeof env[i].value, "%s", value ? value : "");
        return 1;
    }
    if (nenv >= MAX_ENV) return 0;
    snprintf(env[nenv].name, sizeof env[nenv].name, "%s", name);
    snprintf(env[nenv].value, sizeof env[nenv].value, "%s", value ? value : "");
    nenv++;
    return 1;
}
const char *env_get(const char *name)
{
    for (int i = 0; i < nenv; i++) if (!strcmp(env[i].name, name)) return env[i].value;
    return NULL;
}

const char *plat_getenv(const char *name) { return name ? env_get(name) : NULL; }

/* the page serves the data itself: the module asks for "data/pack.pck" and "assets/saber.png" by those names and
 * finds whatever the page handed over (vfs_wasm.c), so there is no path to build */
const char *plat_base_path(void) { return ""; }
const char *plat_default_data_dir(void) { return "data"; }

/* ---- the screen: the canvas is the window ---- */
/* how the page should size the canvas for a SCREEN choice: 0 fills the window, n is a whole number of game
 * pixels across. The page reads these two through wasm_screen_choices(). */
int  plat_screen_modes(void) { return 5; }   /* full, x1, x2, x3, x4 */
void plat_screen_label(int screen, char *buf, size_t n)
{
    static const char *const LABEL[] = { "FULL", "1x", "2x", "3x", "4x" };
    if (!n) return;
    snprintf(buf, n, "%s", screen >= 0 && screen < 5 ? LABEL[screen] : "FULL");
}
bool plat_screen_43_only(int screen) { (void)screen; return false; }   /* the canvas takes any shape */
int  plat_wide_width(int screen) { (void)screen; return WASM_SCREEN_W; }
int  plat_default_ratio(void) { return 0; }   /* WIDE: 426x240 */

void plat_apply_screen(Ren *r, int sw, int sh, int ratio, int screen)
{
    (void)r;
    (void)sw;
    (void)sh;
    (void)ratio;
    (void)screen;   /* the page owns the canvas element: it reads the choice back and resizes it */
}

/* the page draws the canvas, so a screenshot is the page's canvas.toBlob: nothing to do here, and the core's
 * SABER_SHOT path is not something a page can ask for anyway */
bool plat_screenshot(Ren *r, const char *path) { (void)r; (void)path; return false; }
