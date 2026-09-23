#pragma once
/* Platform services the core game uses besides rendering (platform/render.h), sound (platform/aud.h), video
 * (video.h) and input (input.h). Implemented once per platform in platform/<name>/plat_<name>.c. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct Ren Ren;

#define PLAT_CLAMP(x, a, b) ((x) < (a) ? (a) : (x) > (b) ? (b) : (x))

/* PLAT_* feature switches the core may test at compile time are set by the build:
 *   PLAT_DREAMCAST   the KallistiOS build
 *   PLAT_LOW_MEMORY  release pack data once it has been turned into textures (16 MB machines) */

/* debug switches (SABER_*): the environment on PC; a text file of NAME=value lines on consoles (plat_*.c) */
const char *plat_getenv(const char *name);
/* milliseconds since start */
uint64_t plat_ticks_ms(void);
/* directory of the executable with a trailing '/', or NULL */
const char *plat_base_path(void);
/* where the demo's .pck files are when no path is given */
const char *plat_default_data_dir(void);

/* the logical screen: sw x sh game pixels. ratio: 0 wide (426x240, plat_wide_width), -1 4:3 (320x240), 1 stretched to the display.
 * screen: the SCREEN option (0 fullscreen, n = a window n+1 times the game size; Dreamcast: 0 640x480, 1 832x480 on VGA);
 * -1 keeps the current one. */
void plat_apply_screen(Ren *r, int sw, int sh, int ratio, int screen);
/* the SCREEN option's choices and their label ("FULL 852x480", "WINDOWED x2", "VGA 832x480") */
int  plat_screen_modes(void);
void plat_screen_label(int screen, char *buf, size_t n);
/* true when that SCREEN choice only shows 4:3 (the RATIO option is held at 4:3 there) */
bool plat_screen_43_only(int screen);
/* the WIDE ratio's logical width on that SCREEN choice: 426 (852x480 halved), 416 where the display is 832x480 */
int  plat_wide_width(int screen);
/* the ratio a fresh start uses (menu RATIO option) */
int  plat_default_ratio(void);

/* decode an image file (PNG) to RGBA8888 (R first in memory); caller frees; NULL on failure */
uint32_t *plat_image_load_rgba(const char *path, int *w, int *h);
/* save the current frame (debug SABER_SHOT); false where unsupported */
bool plat_screenshot(Ren *r, const char *path);
