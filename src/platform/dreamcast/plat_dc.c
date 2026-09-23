/* platform/plat.h on KallistiOS */
#include "../plat.h"
#include "pvr_internal.h"
#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb_image/stb_image.h>

/* SABER_* debug switches: NAME=value lines in /cd/saber.env (or /pc/saber.env under dcload), '#' comments */
#define MAX_ENV 48
static struct { char name[32], value[1024]; } env[MAX_ENV]; static int nenv = -1;

static void env_load(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[1088];
    while (nenv < MAX_ENV && fgets(line, sizeof line, f)) {
        char *e = strchr(line, '='); if (line[0] == '#' || !e) continue;
        *e = 0; char *v = e + 1; v[strcspn(v, "\r\n")] = 0;
        snprintf(env[nenv].name, sizeof env[nenv].name, "%s", line);
        snprintf(env[nenv].value, sizeof env[nenv].value, "%s", v);
        printf("env %s=%s\n", env[nenv].name, env[nenv].value);
        nenv++;
    }
    fclose(f);
}

const char *plat_getenv(const char *name)
{
    if (nenv < 0) { nenv = 0; env_load("/cd/saber.env"); env_load("/pc/saber.env"); }
    for (int i = 0; i < nenv; i++) if (!strcmp(env[i].name, name)) return env[i].value;
    return NULL;
}

uint64_t plat_ticks_ms(void) { return timer_ms_gettime64(); }
const char *plat_base_path(void) { return "/cd/"; }
const char *plat_default_data_dir(void) { return "/cd/data"; }
int plat_default_ratio(void) { return -1; }   /* 4:3: 320x240 doubled to 640x480, every pixel exact */

/* ---- display modes. KOS's 640x480 and 320x240 (DM_320x240: pixel/line doubled on VGA, 240p NTSC on RGB / S-video /
 * composite; always 4:3), and on a VGA cable 832x480 at 60 Hz, the CVT reduced-blanking v2 timing of the 960x704_Dreamcast sample's
 * STARTUP_832x480_VGA_CVT_RBv2, set through vid_set_mode_ex instead of poking the video registers behind KOS's back; a
 * 416x240 wide screen doubles into it exactly. A switch re-initialises the PVR (its tile buffers are sized from the mode),
 * so it waits for the end of a frame: dc_video_update. */
enum { MODE_640, MODE_832, MODE_320 };
static const struct { int w, h; } MODE_SIZE[3] = { { 640, 480 }, { 832, 480 }, { 320, 240 } };
/* the SCREEN choices for the cable: VGA 640x480 / 832x480 / 320x240, anything else 640x480 / 320x240 */
static int screen_mode(int screen)
{
    if (screen <= 0) return MODE_640;
    if (vid_check_cable() == CT_VGA) return screen == 1 ? MODE_832 : MODE_320;
    return MODE_320;
}
static const pvr_init_params_t PVR_PARAMS = {
    .opb_sizes = { PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_0 },   /* everything is in the translucent list */
    .vertex_buf_size = 512 * 1024,
    .dma_enabled = 0,
    .fsaa_enabled = 0,
    .autosort_disabled = 1,       /* draw in submission order, like the 2D renderer it replaces */
    .opb_overflow_count = 2,
};

/* 909 clocks x 495 lines at the 27 MHz VGA pixel clock = 60.0 Hz; total-1 values, as KOS's own mode table has them */
static const vid_mode_t MODE_832x480 = {
    .width = 832, .height = 480, .flags = 0, .cable_type = CT_VGA, .pm = PM_RGB565,
    .scanlines = 494, .clocks = 908,
    .bitmapx = 69, .bitmapy = 14,
    .scanint1 = 14, .scanint2 = 247,          /* vblank irq lines 14 / 494 (KOS doubles scanint2 on VGA) */
    .borderx1 = 69, .borderx2 = 901,
    .bordery1 = 14, .bordery2 = 494,
    .fb_curr = 0, .fb_count = 1, .fb_size = 832 * 480 * 2,
};
/* the two timing registers vid_set_mode_ex leaves alone (they keep the boot ROM's 640x480 values) */
#define REG_SYNC_WIDTH 0x00e0                 /* SPG_WIDTH: hsync / vsync / equalising / broad sync widths */
#define SYNC_WIDTH_832 0x01f6d81e             /* 7, 877, vsync 8 lines, hsync 31 clocks */
#define HPOS_IRQ_832   0x03850000             /* hblank irq at clock 901 */
static uint32_t boot_sync_width, boot_hpos_irq;
static int cur_mode, want_mode = -1;
static struct { int sw, sh, ratio; } view_args = { 320, 240, -1 };
static void apply_view(void);

static void set_mode(int mode)
{
    vid_set_enabled(false);
    if (mode == MODE_832) {
        vid_mode_t m = MODE_832x480;          /* vid_set_mode_ex edits its argument */
        PVR_SET(REG_SYNC_WIDTH, SYNC_WIDTH_832); PVR_SET(PVR_HPOS_IRQ, HPOS_IRQ_832);
        vid_set_mode_ex(&m);
    } else {
        PVR_SET(REG_SYNC_WIDTH, boot_sync_width); PVR_SET(PVR_HPOS_IRQ, boot_hpos_irq);
        vid_set_mode(mode == MODE_320 ? DM_320x240 : DM_640x480, PM_RGB565);
    }
    cur_mode = mode;
}

void dc_video_init(void)
{
    boot_sync_width = PVR_GET(REG_SYNC_WIDTH); boot_hpos_irq = PVR_GET(PVR_HPOS_IRQ);
    set_mode(MODE_640);
    pvr_init(&PVR_PARAMS);
    pvr_set_bg_color(0, 0, 0);
}

/* between frames: carry out a SCREEN change asked for by plat_apply_screen */
void dc_video_update(void)
{
    if (want_mode < 0 || want_mode == cur_mode) { want_mode = -1; return; }
    int mode = want_mode; want_mode = -1;
    pvr_wait_ready(); pvr_wait_render_done(); /* the last scene is rendered, nothing reads VRAM any more */
    if (!rdc_vram_park()) { apply_view(); return; }
    pvr_shutdown();
    set_mode(mode);
    if (vid_mode->width != MODE_SIZE[mode].w) { printf("video: %dx%d refused, back to 640x480\n", MODE_SIZE[mode].w, MODE_SIZE[mode].h); set_mode(MODE_640); }
    pvr_init(&PVR_PARAMS);
    pvr_set_bg_color(0, 0, 0);
    rdc_vram_unpark();
    printf("video: %dx%d\n", vid_mode->width, vid_mode->height);
    apply_view();
}

int plat_screen_modes(void) { return vid_check_cable() == CT_VGA ? 3 : 2; }
bool plat_screen_43_only(int screen) { return screen_mode(screen) == MODE_320; }   /* 320x240 is never wide */
int plat_wide_width(int screen) { return screen_mode(screen) == MODE_832 ? 416 : 426; }
void plat_screen_label(int screen, char *buf, size_t n)
{
    int c = vid_check_cable(), m = screen_mode(screen);
    snprintf(buf, n, "%s %dx%d", c == CT_VGA ? "VGA" : c == CT_RGB ? "RGB" : "TV", MODE_SIZE[m].w, MODE_SIZE[m].h);
}

/* the logical screen -> the display that is up now */
static void apply_view(void)
{
    int sw = view_args.sw, sh = view_args.sh;
    float dw = vid_mode->width, dh = vid_mode->height;
    pvr_view.lw = sw; pvr_view.lh = sh;
    if (view_args.ratio == 1) { pvr_view.sx = dw / sw; pvr_view.sy = dh / sh; pvr_view.ox = pvr_view.oy = 0; }   /* stretched */
    else {   /* the largest scale that fits, centred (4:3: exactly 2x, 1x in 320x240; wide: 2x in 832x480, 1.5x letterboxed in
              * 640x480; 320x240 is always 4:3) */
        float s = dw / sw < dh / sh ? dw / sw : dh / sh;
        if (s >= 2.0f) s = (float)(int)s; else if (s > 1.5f) s = 1.5f;
        pvr_view.sx = pvr_view.sy = s;
        pvr_view.ox = (dw - sw * s) * 0.5f; pvr_view.oy = (dh - sh * s) * 0.5f;
    }
    extern void rdc_view_changed(void);
    rdc_view_changed();
}

/* screen < 0: keep the display mode. A new mode (and the view for it) waits for the end of the frame being drawn
 * (dc_video_update), or is set right away outside one (a SABER_SCREEN start, before anything is in VRAM). */
void plat_apply_screen(Ren *r, int sw, int sh, int ratio, int screen)
{
    (void)r;
    view_args.sw = sw; view_args.sh = sh; view_args.ratio = ratio;
    int mode = screen_mode(screen);
    if (screen >= 0 && mode != cur_mode) { want_mode = mode; if (!rdc_in_frame()) dc_video_update(); }
    else apply_view();
}

uint32_t *plat_image_load_rgba(const char *path, int *w, int *h)
{
    int n;
    unsigned char *px = stbi_load(path, w, h, &n, 4);
    return (uint32_t *)px;   /* stb allocates with malloc: the caller's free() is right */
}

bool plat_screenshot(Ren *r, const char *path) { (void)r; return vid_screen_shot(path) == 0; }
