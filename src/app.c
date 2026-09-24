#include "app.h"
#include "pack.h"
#include "gfx.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Game g;   /* static: several hundred KB, too big for a console's main stack */
static const char *script; static int script_n; static char script_keys[16];
static int shot_frames = -1; static char shot_path[256];
#ifndef REAL_FIXED
static double acc;
#endif
static int acc_fields;
static bool shot_now;

Game *app_game(void) { return &g; }

bool app_init(Ren *ren, const char *data_dir, int start_level)
{
    gfx_init(ren);
#ifdef PLAT_STAGE_PACK
    /* blocks baked for the console that replace the demo's (the Saturn: a level without the tile maps its planes draw,
     * tools/saturn/build_disc.py): opened first, so a lookup finds them before the demo's */
    static const char *const over[] = { "stage.pck" };
    if (!packs_open(data_dir, over, 1)) return false;
#endif
    static const char *const base[] = { "pack.pck", "common.pck", "levels.pck", "menu.pck", "level1.pck", "video.pck" };
    if (!packs_open(data_dir, base, 6)) return false;
#ifdef PLAT_BAKED_ASSETS
    /* our assets and the packs' graphics and sounds, baked for the console by tools/dc/build_disc.py */
    static const char *const baked[] = { "tex.pck", "snd.pck", "files.pck" };
    if (!packs_open(data_dir, baked, 3)) return false;
#endif
    audio_init();
    if (!game_init(&g, ren, APP_SCREEN_W, APP_SCREEN_H, start_level)) return false;
    /* debug: SABER_SCRIPT="60:R,20:RJ,40:" drives the input for N fixed steps each (L R U D J S A P, X = power) */
    script = plat_getenv("SABER_SCRIPT");
    /* debug: SABER_SHOT=path,camx,steps -> save a screenshot after N fixed steps and quit */
    if (plat_getenv("SABER_SHOT")) {
        const char *s = plat_getenv("SABER_SHOT"), *c = strchr(s, ','), *e = NULL;
        size_t len = c ? (size_t)(c - s) : strlen(s); if (len > 255) len = 255;
        memcpy(shot_path, s, len); shot_path[len] = 0;
        real cx = R(-1); int n = 1;
        if (c) { cx = r_parse(c + 1, &e); if (*e == ',') n = atoi(e + 1); }
        if (cx >= 0) g.cam_x = cx;
        shot_frames = n;
    }
    return true;
}

static void script_step(void)
{
    if (script_n == 0 && *script) {
        int used = 0; sscanf(script, "%d:%15[A-Z]%n", &script_n, script_keys, &used);
        if (used == 0) { sscanf(script, "%d:%n", &script_n, &used); script_keys[0] = 0; }
        if (used == 0) { script = ""; return; }
        script += used; if (*script == ',') script++;
        if (script_n <= 0) script_n = 1;
    }
    if (script_n > 0) {
        script_n--;
        memset(g.in.raw, 0, sizeof g.in.raw);
        for (const char *k = script_keys; *k; k++) switch (*k) {
            case 'L': g.in.raw[BTN_LEFT] = true; break;  case 'R': g.in.raw[BTN_RIGHT] = true; break;
            case 'U': g.in.raw[BTN_UP] = true; break;    case 'D': g.in.raw[BTN_DOWN] = true; break;
            case 'J': g.in.raw[BTN_JUMP] = true; break;  case 'S': g.in.raw[BTN_SHOOT] = true; break;
            case 'A': g.in.raw[BTN_AIM] = true; break;   case 'P': g.in.raw[BTN_PAUSE] = true; break;
            case 'X': g.in.raw[BTN_POWER] = true; break; }
    }
}

static int fast = -1;
static bool fuzz, fuzz_read;
static void one_step(void);

#ifndef REAL_FIXED
int app_update(double elapsed)
{
    int steps = 0;
    const double step = 1.0 / 60.0;
    acc += elapsed;
    if (acc > 0.25) acc = 0.25;
    if (fast < 0) fast = plat_getenv("SABER_FAST") ? atoi(plat_getenv("SABER_FAST")) : 0;
    if (fast > 0) acc = step * fast;   /* debug: n fixed steps per drawn frame (long headless runs) */
    while (acc >= step) { one_step(); acc -= step; steps++; }
    return steps;
}
#endif

int app_update_fields(int fields)
{
    int steps = 0;
    acc_fields += fields;
    if (acc_fields > 15) acc_fields = 15;   /* a quarter of a second */
    if (fast < 0) fast = plat_getenv("SABER_FAST") ? atoi(plat_getenv("SABER_FAST")) : 0;
    if (fast > 0) acc_fields = fast;
    while (acc_fields >= 1) { one_step(); acc_fields--; steps++; }
    return steps;
}

static void one_step(void)
{
    if (!fuzz_read) { fuzz = plat_getenv("SABER_FUZZ") != NULL; fuzz_read = true; }
    {
        if (script) script_step();
        if (fuzz) {   /* debug: random inputs, hold each for a few frames */
            static int hold; static unsigned mask;
            if (hold-- <= 0) { hold = rand() % 20; mask = (unsigned)rand(); }
            for (int b = 0; b < BTN_COUNT; b++) g.in.raw[b] = (mask >> b) & 1;
            if (g.in.raw[BTN_LEFT] && g.in.raw[BTN_RIGHT]) g.in.raw[BTN_LEFT] = false;
            g.in.raw[BTN_PAUSE] = false; g.in.raw[BTN_POWER] = false;
        }
        game_update(&g, R_DT); audio_update();
        if (shot_frames > 0) shot_frames--;   /* SABER_SHOT counts fixed steps, not rendered frames */
    }
}

void app_draw(void)
{
    r_set_draw_color(g.ren, 0, 0, 0, 255);
    r_clear(g.ren);
    gfx_frame();
    game_draw(&g);
    shot_now = shot_frames == 0 && shot_path[0];
}

const char *app_shot_path(void) { return shot_now ? shot_path : NULL; }

bool app_perf_on(void)
{
    static int on = -1; if (on < 0) on = plat_getenv("SABER_PERF") != NULL;
    return on;
}

static char *ms2(char *buf, uint32_t us)   /* microseconds as milliseconds with 2 decimals */
{
    uint32_t h = (us + 5) / 10;
    snprintf(buf, 12, "%u.%02u", (unsigned)(h / 100), (unsigned)(h % 100));
    return buf;
}

void app_perf(uint32_t upd_us, uint32_t draw_us, uint32_t frame_us, int steps, int prims)
{
    static uint32_t n, su, sd, mu, md, mf, prims_max, skip, dbl, quiet; static unsigned reads0;
    if (quiet) { quiet--; return; }   /* the frames the report itself held up (a serial console line takes ~17 ms) */
    su += upd_us; sd += draw_us;
    if (upd_us > mu) mu = upd_us;
    if (draw_us > md) md = draw_us;
    if (frame_us > mf) mf = frame_us;
    if (prims > (int)prims_max) prims_max = (uint32_t)prims;
    if (steps == 0) skip++; else if (steps > 1) dbl++;
    if (++n < 60) return;
    unsigned reads = packs_reads();
    char a[12], b[12], c[12], d[12], e[12];
    fprintf(stderr, "perf: cam %5s enemies %2d | update %5s/%5s ms | draw %5s/%5s ms | frame max %5s ms | "
                    "steps 0x%u 2+x%u | prims %u | pack reads %u\n",
            RS(g.in_level ? g.cam_x : R(-1), 0), g.enemies.count, ms2(a, su / 60), ms2(b, mu), ms2(c, sd / 60), ms2(d, md), ms2(e, mf),
            skip, dbl, prims_max, reads - reads0);
    reads0 = reads; n = su = sd = mu = md = mf = prims_max = skip = dbl = 0; quiet = 2;
}

void app_shutdown(void)
{
    audio_shutdown();
    packs_close();
}
