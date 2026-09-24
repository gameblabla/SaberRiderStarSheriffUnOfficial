/* A headless host build of the game (Makefile.headless): the core on the null renderer, audio and video, driven by the
 * debug script (SABER_SCRIPT) at a fixed 60 Hz with simulated time, so two builds given the same input run the same
 * frames. For regression checks of arithmetic changes: SABER_TRACE=3 (every step of the platform stages) and
 * SABER_DRAWLOG=file (every draw call of every frame, render_null.c).
 *   saber_headless <data_dir> [start_level]      SABER_FRAMES=n: frames to run (default 3600) */
#include "../../app.h"
#include "../plat.h"
#include "../../input.h"
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Ren *rnull_renderer(void);
void rnull_frame_end(unsigned frame);

static unsigned frame;

const char *plat_getenv(const char *name) { return getenv(name); }
uint64_t plat_ticks_ms(void) { return (uint64_t)frame * 1000u / 60u; }
const char *plat_base_path(void) { return NULL; }
const char *plat_default_data_dir(void) { return "SaberRider/data"; }
void plat_apply_screen(Ren *r, int sw, int sh, int ratio, int screen) { (void)r; (void)sw; (void)sh; (void)ratio; (void)screen; }
int  plat_screen_modes(void) { return 1; }
void plat_screen_label(int screen, char *buf, size_t n) { (void)screen; snprintf(buf, n, "HEADLESS"); }
bool plat_screen_43_only(int screen) { (void)screen; return false; }
int  plat_wide_width(int screen) { (void)screen; return APP_SCREEN_W; }
int  plat_default_ratio(void) { return 0; }
bool plat_screenshot(Ren *r, const char *path) { (void)r; (void)path; return false; }

uint32_t *plat_image_load_rgba(const char *path, int *w, int *h)
{
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path)) return NULL;
    img.format = PNG_FORMAT_RGBA;   /* bytes R, G, B, A: the RGBA8888 render.h asks for */
    uint32_t *px = malloc(PNG_IMAGE_SIZE(img));
    if (!px || !png_image_finish_read(&img, NULL, px, 0, NULL)) { free(px); png_image_free(&img); return NULL; }
    *w = (int)img.width; *h = (int)img.height;
    return px;
}

void plat_input_poll(bool down[BTN_COUNT]) { (void)down; }
bool plat_bind_supported(void) { return false; }
void plat_bind_label(int dev, int btn, int slot, char *buf, size_t n) { (void)dev; (void)btn; (void)slot; if (n) buf[0] = 0; }
void plat_bind_capture(int dev, int btn, int slot) { (void)dev; (void)btn; (void)slot; }
bool plat_bind_capturing(void) { return false; }
void plat_bind_cancel(void) { }
void plat_bind_defaults(int dev) { (void)dev; }
void plat_bind_save(void) { }

int main(int argc, char **argv)
{
    const char *data = argc > 1 ? argv[1] : plat_default_data_dir();
    int level = argc > 2 ? atoi(argv[2]) : 0;
    unsigned frames = getenv("SABER_FRAMES") ? (unsigned)atoi(getenv("SABER_FRAMES")) : 3600u;
    if (!app_init(rnull_renderer(), data, level)) { fprintf(stderr, "app_init failed\n"); return 1; }
    for (frame = 0; frame < frames; frame++) {
        app_update_fields(1);
        app_draw();
        rnull_frame_end(frame);
    }
    app_shutdown();
    return 0;
}
