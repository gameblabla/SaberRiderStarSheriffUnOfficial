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
int plat_screen_modes(void) { return 1; }
void plat_screen_label(int screen, char *buf, size_t n)
{
    (void)screen;
    int c = vid_check_cable();
    snprintf(buf, n, "%s", c == CT_VGA ? "VGA 640x480" : c == CT_RGB ? "RGB 640x480" : "TV 640x480");
}

void plat_apply_screen(Ren *r, int sw, int sh, int ratio, int screen)
{
    (void)r; (void)screen;
    pvr_view.lw = sw; pvr_view.lh = sh;
    if (ratio == 1) { pvr_view.sx = 640.0f / sw; pvr_view.sy = 480.0f / sh; pvr_view.ox = pvr_view.oy = 0; }   /* stretched */
    else {   /* the largest scale that fits, centred (4:3: exactly 2x; wide: 1.5x letterboxed) */
        float s = 640.0f / sw < 480.0f / sh ? 640.0f / sw : 480.0f / sh;
        if (s >= 2.0f) s = (float)(int)s; else if (s > 1.5f) s = 1.5f;
        pvr_view.sx = pvr_view.sy = s;
        pvr_view.ox = (640.0f - sw * s) * 0.5f; pvr_view.oy = (480.0f - sh * s) * 0.5f;
    }
    extern void rdc_view_changed(void);
    rdc_view_changed();
}

uint32_t *plat_image_load_rgba(const char *path, int *w, int *h)
{
    int n;
    unsigned char *px = stbi_load(path, w, h, &n, 4);
    return (uint32_t *)px;   /* stb allocates with malloc: the caller's free() is right */
}

bool plat_screenshot(Ren *r, const char *path) { (void)r; return vid_screen_shot(path) == 0; }
