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
#ifdef SAT_DIAG
    if (!strcmp(name, "SABER_MENU")) return "1";   /* the diagnostic disc starts on the logo splashes, before the intro FMV */
#endif
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

/* counts * 1000 / COUNT_1MS as a multiply by a 16.16 constant (a 64-bit division is a libgcc call of hundreds of
 * cycles); counts stays under 2^40 for days. The 352 modes run the clock at 28.64 MHz (sat_timer_clock). */
static uint64_t us_per_count = (1000ull << 16) / CPU_FRT_NTSC_320_32_COUNT_1MS;
static uint32_t us_base;   /* the time when the counter last restarted (a clock change) */

uint32_t sat_timer_us(void)
{
    uint32_t hi, lo;
    do { hi = frt_high; lo = cpu_frt_count_get(); } while (hi != frt_high);
    return us_base + (uint32_t)(((((uint64_t)hi << 16) | lo) * us_per_count) >> 16);
}

/* a wait that needs no interrupt (the FRT's overflow one included): its counter's steps, added up, at the 352 or 320
 * modes' clock */
void sat_busy_wait_us(uint32_t us, bool mode352)
{
    uint32_t want = (uint32_t)(((uint64_t)us * (mode352 ? CPU_FRT_NTSC_352_32_COUNT_1MS : CPU_FRT_NTSC_320_32_COUNT_1MS)) / 1000u), got = 0;
    uint16_t last = cpu_frt_count_get();
    while (got < want) { uint16_t now = cpu_frt_count_get(); got += (uint16_t)(now - last); last = now; }
}

/* the system clock changed (render_sat.c rsat_set_mode): the time goes on from now_us (what it was before the change,
 * plus the change's own time), counted at the new rate */
void sat_timer_clock(bool mode352, uint32_t now)
{
    uint32_t sr = cpu_intc_mask_get(); cpu_intc_mask_set(15);
    us_per_count = (1000ull << 16) / (mode352 ? CPU_FRT_NTSC_352_32_COUNT_1MS : CPU_FRT_NTSC_320_32_COUNT_1MS);
    cpu_frt_init(CPU_FRT_CLOCK_DIV_32);   /* the BIOS's clock change leaves the FRT at its reset setting (clock / 8, no
                                           * overflow interrupt) */
    cpu_frt_ovi_set(frt_overflow);
    frt_high = 0; cpu_frt_count_set(0); us_base = now;
    cpu_intc_mask_set(sr);
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
void plat_screen_label(int screen, char *buf, size_t n) { (void)screen; snprintf(buf, n, "%dx%d", view.sw, SAT_SCREEN_H); }
void plat_apply_screen(Ren *r, int sw, int sh, int ratio, int screen)
{
    (void)r; (void)screen;
    view.sw = sw; view.sh = sh; view.ratio = ratio;
#ifndef SAT_RENDER_NULL
    rsat_set_mode(sw == SAT_WIDE_W);   /* the TV mode (and system clock) for that width */
    rsat_set_screen(sw, sh);
#endif
}

/* no image decoding on the console: the disc's images are baked (PLAT_BAKED_ASSETS, assets.c) */
uint32_t *plat_image_load_rgba(const char *path, int *w, int *h) { (void)path; (void)w; (void)h; return NULL; }
bool plat_screenshot(Ren *r, const char *path) { (void)r; (void)path; return false; }
