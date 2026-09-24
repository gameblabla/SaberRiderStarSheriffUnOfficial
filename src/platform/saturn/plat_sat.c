/* platform/plat.h on the Saturn (libyaul) */
#include "../plat.h"
#include "sat_internal.h"
#include <yaul.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SABER_* debug switches: NAME=value lines in SABER.ENV on the disc, '#' comments (as the Dreamcast's saber.env) */
#define MAX_ENV 32
static struct { char name[32], value[256]; } env[MAX_ENV]; static int nenv = -1;

static void env_load(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[300];
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
    if (nenv < 0) { nenv = 0; env_load("SABER.ENV"); }
    for (int i = 0; i < nenv; i++) if (!strcmp(env[i].name, name)) return env[i].value;
    return NULL;
}

/* ---- time: the FRT counts at the system clock / 32 (26.87 MHz in the 320 modes: 0x348 counts a millisecond); its
 * 16-bit counter overflows every ~78 ms into `frt_high` */
static volatile uint32_t frt_high, vblanks;
static void frt_overflow(void) { frt_high++; }

void sat_timer_init(void)
{
    cpu_frt_init(CPU_FRT_CLOCK_DIV_32);
    cpu_frt_ovi_set(frt_overflow);
    cpu_frt_count_set(0);
}

uint32_t sat_timer_us(void)
{
    uint32_t hi, lo;
    do { hi = frt_high; lo = cpu_frt_count_get(); } while (hi != frt_high);
    uint64_t counts = ((uint64_t)hi << 16) | lo;
    return (uint32_t)(counts * 1000u / CPU_FRT_NTSC_320_32_COUNT_1MS);
}

void sat_vblank_tick(void) { vblanks++; }
uint32_t sat_vblanks(void) { return vblanks; }

uint64_t plat_ticks_ms(void) { return sat_timer_us() / 1000u; }
const char *plat_base_path(void) { return ""; }
const char *plat_default_data_dir(void) { return ""; }

/* ---- the screen: no SCREEN choices; RATIO picks 320x224 (4:3) or 352x224 (wide) */
static struct { int sw, sh, ratio; } view = { 320, SAT_SCREEN_H, -1 };
int  plat_default_ratio(void) { return -1; }
int  plat_screen_modes(void) { return 1; }
bool plat_screen_43_only(int screen) { (void)screen; return false; }
int  plat_wide_width(int screen) { (void)screen; return SAT_WIDE_W; }
void plat_screen_label(int screen, char *buf, size_t n) { (void)screen; snprintf(buf, n, "TV %dx%d", view.sw, SAT_SCREEN_H); }
void plat_apply_screen(Ren *r, int sw, int sh, int ratio, int screen)
{
    (void)r; (void)screen;
    view.sw = sw; view.sh = sh; view.ratio = ratio;
#ifndef SAT_RENDER_NULL
    rsat_set_screen(sw, sh);
#endif
}

/* no image decoding on the console: the disc's images are baked (PLAT_BAKED_ASSETS, assets.c) */
uint32_t *plat_image_load_rgba(const char *path, int *w, int *h) { (void)path; (void)w; (void)h; return NULL; }
bool plat_screenshot(Ren *r, const char *path) { (void)r; (void)path; return false; }
