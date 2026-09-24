#include "mode7.h"
#include "assets.h"
#include "gfx.h"
#include "font.h"
#include "audio.h"
#include "dialog.h"
#include "heroes.h"
#include "namehash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI R(3.1415927f)
#define TWO_PI R(6.2831853f)

/* ---- projection ---- */
#define HORIZON 112          /* screen row of the horizon (floor rows HORIZON+1 .. sh-1) */
#define CAM_H R(48.0f)          /* camera height in world units */
#define FOCAL R(210.0f)         /* pixels */
#define CAM_BACK R(92.0f)       /* the player's ground point is this far in front of the camera */
#define FOG0 R(1400.0f)
#define FOG1 R(3400.0f)

/* ---- world ---- */
/* The floor is a material map at 8 world units per cell (wraps at 8192 units); every material is a 32x32 texture sampled
 * by world position, so a material continues seamlessly across cells and the road's edges/kerbs can follow the track
 * at 8-unit resolution instead of 32-unit tiles. Textures carry 4 mip levels against far-row shimmer. */
#define MAPN 1024            /* cells per side (wraps) */
#define MAPSH 3              /* log2(cell size): 8 units */
#define WORLD (MAPN << MAPSH)
#define TEX 32               /* material texture size */
#define MIPS 4
enum { T_SAND, T_SAND2, T_ASPHALT, T_LINE, T_KERB_RED, T_KERB_WHITE, T_CHECKER, T_DIRT, T_SAND_DARK, T_DASH, T_SHOULDER, T_COUNT };

/* ---- atlas ---- */
enum { S_BUGGY, S_HORNET, S_LEADER, S_FIRENZA, S_RBLUE, S_RPURPLE, S_CACTUS, S_ROCK_S, S_ROCK_B, S_MESA,
       S_FLOOR, S_SHOT, S_ESHOT, S_EXPL, S_FLASH, S_MINE, S_GATE, S_SMOKE, S_TURBO, S_COUNT };
static const char *const SPR_NAMES[S_COUNT] = { "buggy", "hornet", "leader", "firenza", "racer_blue", "racer_purple",
    "cactus", "rock_small", "rock_big", "mesa", "floor", "shot", "eshot", "explosion", "flash", "mine", "gate", "smoke", "turbo" };
typedef struct { int x, y, w, h, frames; } Spr;

/* ---- entities ---- */
enum { K_NONE, K_RACER, K_ESCORT, K_PROP, K_SHOT, K_ESHOT, K_MINE, K_EXPL, K_BOSS, K_GATE, K_SMOKE, K_FLASH };
/* boss states: the Hornet leader flees up the desert road during the pursuit, then (Chase H.Q. style) keeps
 * running on the road while you shoot and ram him, and finally burns out */
enum { B_FLEE = 10, B_RUN = 20, B_DYING = 30 };
typedef struct {
    int kind, spr, frame; real anim;
    real x, y, z, heading, speed, vx, vy;
    real hp, hp_max, t, t2, t3, gun_t, scale, tilt;
    int state; bool player_owned, hornet, solid;
    /* racers: on rails along the track */
    real s, lat, lat_target, max_speed; int lap, id; real knock;
    uint8_t r, g, b;   /* colour mod */
} Ent;
#define MAX_ENT 200

enum { PH_INTRO, PH_INSTRUCTIONS, PH_COUNTDOWN, PH_RACE, PH_FINISH, PH_BREAKAWAY, PH_BRIEF, PH_PURSUIT, PH_CAUGHT, PH_BOSS, PH_VICTORY, PH_DEAD, PH_GAMEOVER, PH_CLEARED };
/* the chequered flag -> the Hornets' breakaway -> the Chase H.Q. style target briefing -> zoom into the pursuit */
#define FINISH_DUR R(3.2f)
#define BRIEF_TEXT_DUR R(5.2f)   /* the briefing's lines have typed in and held */
#define BRIEF_ZOOM_DUR R(0.9f)   /* the target zooms into the camera under a white-out */
#define PURSUIT_FADE R(1.1f)     /* the desert fades in from white */
#define CATCH_GAP R(260.0f)     /* the pursuit ends when the leader is this close */
#define GAP_MAX R(4200.0f)      /* HUD gap bar full scale */
#define INSTR_OPEN_DUR R(0.5f)   /* the controls card grows/fades in before it can be skipped */
#define INSTR_MIN_DUR R(2.2f)    /* minimum time on screen even with no input, so it isn't a flash-frame */

#define TRACK_N 1024
/* race progress (laps x track length + s) and squared distances: 64 bits in fixed point (helpers below) */
#ifdef REAL_FIXED
typedef int64_t rprog;
typedef int64_t rdist2;
#else
typedef float rprog;
typedef float rdist2;
#endif
#define N_RACERS 7

struct Mode7 {
    Ren *ren; int sw, sh;
    RTex *atlas; Spr spr[S_COUNT]; bool ok;
    uint32_t tiles[T_COUNT][MIPS][TEX * TEX];   /* level L is (TEX >> L) square */
    uint8_t cells[MAPN * MAPN];
    RFloor *floor; int floor_h;   /* the ground plane (platform/render.h), made on first draw */
    RTex *sky_tex; int sky_w, sky_h; bool sky_ok;
    /* track */
    real tx[TRACK_N], ty[TRACK_N], tlen[TRACK_N], track_len;
    /* player */
    real px, py, heading, speed, cam_heading, vx, vy;
    int hp, max_hp, lives, difficulty;
    real boost, fire_cd, hurt_t, spin_t, spin_dur, spin_heading, bounce, anim_t, shake, tilt;
    real s, lat; int lap, rank, near_idx; rprog progress; real last_s;
    /* phase */
    int phase; real phase_t, countdown;
    Dialog dlg; bool paused; int result;
    bool fire_hold;            /* set through a dialogue: no shot until the shoot button (used to page it) is let go */
    Ent ents[MAX_ENT];
    /* pursuit */
    real gap, pursuit_spawn_t, pursuit_t;
    /* boss */
    real boss_hp_max; int boss_i; real ram_cd; real wreck_x, wreck_y, wreck_t;
    real race_time; int kills;
    unsigned rng;
    real dead_t; int resume_phase;
    char msg[64]; real msg_t;
    int music_now;
    bool intro_pending;        /* the intro dialog opens on the first update (after the level title card) */
    bool turbo_on; real turbo_t; int finish_rank;   /* turbo: lit last frame / flame animation clock; finish: the placing at the flag */
    bool boost_locked;   /* turbo used up: no boost again until the meter recharges halfway */
    real white;               /* full-screen white veil alpha (the zoom into the pursuit) */
};

/* race progress, laps x the track's length plus s (13205 units a lap): fixed point keeps it in 64 bits, where three
 * laps would overflow 16.16; squared distances likewise (dist2) */
#ifdef REAL_FIXED
#define PROG(lap, s) ((int64_t)(lap) * m->track_len + (s))
#define DIST2_MAX INT64_MAX
static int prog_lap(rprog p, real len) { return (int)(p >= 0 ? p / len : -((-p + len - 1) / len)); }   /* floor */
static rdist2 dist2(real dx, real dy) { return (int64_t)dx * dx + (int64_t)dy * dy; }
#else
#define PROG(lap, s) ((lap) * m->track_len + (s))
#define DIST2_MAX 1e30f
static int prog_lap(rprog p, real len) { return (int)floorf(p / len); }
static rdist2 dist2(real dx, real dy) { return dx * dx + dy * dy; }
#endif
/* a racer's distance ahead of the player (negative: behind), clamped to +-30000 in fixed point */
#define PROG_GAP(e) prog_gap(PROG((e)->lap, (e)->s) - m->progress)
#ifdef REAL_FIXED
static real prog_gap(rprog d) { return d > FX(30000) ? FX(30000) : d < -FX(30000) ? -FX(30000) : (real)d; }
#else
static real prog_gap(rprog d) { return d; }
#endif

/* ---------------------------------------------------------------- helpers */
#ifdef REAL_FIXED
static real frand(Mode7 *m) { m->rng = m->rng * 1664525u + 1013904223u; return (real)(m->rng >> 16); }   /* 16 bits of fraction */
#else
static float frand(Mode7 *m) { m->rng = m->rng * 1664525u + 1013904223u; return (m->rng >> 8) / 16777216.0f; }
#endif
static real wrapf(real v) { v = r_fmod(v, r_int(WORLD)); return v < 0 ? v + r_int(WORLD) : v; }
static real angdiff(real a, real b) { real d = r_fmod(a - b + PI, TWO_PI); if (d < 0) d += TWO_PI; return d - PI; }
static real clampf(real v, real lo, real hi) { return v < lo ? lo : v > hi ? hi : v; }
/* shortest wrapped delta */
static real dwrap(real a, real b) { real d = a - b; if (d > r_int(WORLD / 2)) d -= r_int(WORLD); if (d < r_int(-WORLD / 2)) d += r_int(WORLD); return d; }
/* hundredths of a second (the race clock; fixed point would overflow t * 100 past 327 s) */
#ifdef REAL_FIXED
static int centis(real t) { return (int)(((int64_t)t * 100) >> 16); }
#else
static int centis(float t) { return (int)(t * 100); }
#endif
static void set_msg(Mode7 *m, const char *s, real t) { snprintf(m->msg, sizeof m->msg, "%s", s); m->msg_t = t; }
static void play_music(Mode7 *m, int track, bool loop) { if (m->music_now != track) { music_play(track, loop); m->music_now = track; music_set_volume(R(1.0f)); } }

static Ent *ent_new(Mode7 *m)
{
    for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_NONE) { Ent *e = &m->ents[i]; memset(e, 0, sizeof *e); e->scale = R(1); e->r = e->g = e->b = 255; return e; }
    return NULL;
}
static void ents_clear(Mode7 *m, bool keep_player_stuff) { (void)keep_player_stuff; memset(m->ents, 0, sizeof m->ents); }

static uint8_t cell_at(const Mode7 *m, real x, real y) { int cx = (r_floor(x) >> MAPSH) & (MAPN - 1), cy = (r_floor(y) >> MAPSH) & (MAPN - 1); return m->cells[cy * MAPN + cx]; }
static void cell_set(Mode7 *m, int cx, int cy, uint8_t t) { m->cells[(cy & (MAPN - 1)) * MAPN + (cx & (MAPN - 1))] = t; }
static bool is_road(uint8_t t) { return t == T_ASPHALT || t == T_LINE || t == T_CHECKER || t == T_DASH || t == T_DIRT; }
static bool is_rumble(uint8_t t) { return t == T_KERB_RED || t == T_KERB_WHITE || t == T_SHOULDER; }

static void spawn_expl(Mode7 *m, real x, real y, real scale)
{
    Ent *e = ent_new(m); if (!e) return;
    e->kind = K_EXPL; e->spr = S_EXPL; e->x = x; e->y = y; e->scale = scale; e->t = 0;
}
static void spawn_smoke(Mode7 *m, real x, real y, real z)
{
    Ent *e = ent_new(m); if (!e) return;
    e->kind = K_SMOKE; e->spr = S_SMOKE; e->x = x; e->y = y; e->z = z; e->scale = R(1);
}

/* ---------------------------------------------------------------- atlas */
static bool load_atlas(Mode7 *m)
{
    char txt[1024]; const char *p = asset_path("mode7.txt");   /* asset_path returns a static buffer */
    if (!p) { fprintf(stderr, "assets/mode7.txt missing (run tools/build_mode7_assets.py)\n"); return false; }
    snprintf(txt, sizeof txt, "%s", p);
    const char *png = asset_path("mode7.png");
    if (!png) { fprintf(stderr, "assets/mode7.png missing (run tools/build_mode7_assets.py)\n"); return false; }
    int w, h; uint32_t *px = png_load_rgba(png, &w, &h);
    if (!px) return false;
    FILE *f = asset_fopen(txt);
    char name[64]; int x, y, sw, sh, n; int found = 0;
    while (f && fscanf(f, "%63s %d %d %d %d %d", name, &x, &y, &sw, &sh, &n) == 6) {
        for (int i = 0; i < S_COUNT; i++) if (!strcmp(name, SPR_NAMES[i])) { m->spr[i] = (Spr){ x, y, sw, sh, n }; found++; }
    }
    if (f) fclose(f);
    if (found < S_COUNT) fprintf(stderr, "mode7.txt: %d/%d sprites\n", found, S_COUNT);
    m->atlas = NULL;
#ifdef PLAT_BAKED_ASSETS
    m->atlas = gfx_image_tex(png, NULL, NULL);   /* the console's baked texture */
#endif
    if (!m->atlas) m->atlas = rtex_create(m->ren, w, h, R_TEX_STATIC, px);
    rtex_set_blend(m->atlas, R_BLEND_BLEND); rtex_set_scale(m->atlas, R_SCALE_NEAREST);
    /* floor materials into memory, with box-filtered mips */
    Spr *fl = &m->spr[S_FLOOR];
    for (int t = 0; t < T_COUNT && t < fl->frames; t++) {
        for (int yy = 0; yy < TEX; yy++)
            memcpy(m->tiles[t][0] + yy * TEX, px + (size_t)(fl->y + yy) * w + fl->x + t * fl->w, TEX * 4);
        for (int L = 1; L < MIPS; L++) {
            int n = TEX >> L, pn = TEX >> (L - 1); const uint32_t *src = m->tiles[t][L - 1]; uint32_t *dst = m->tiles[t][L];
            for (int yy = 0; yy < n; yy++) for (int xx = 0; xx < n; xx++) {
                uint32_t c[4] = { src[(2 * yy) * pn + 2 * xx], src[(2 * yy) * pn + 2 * xx + 1], src[(2 * yy + 1) * pn + 2 * xx], src[(2 * yy + 1) * pn + 2 * xx + 1] };
                uint32_t r = 0, g = 0, b = 0;
                for (int k = 0; k < 4; k++) { r += c[k] & 0xff; g += (c[k] >> 8) & 0xff; b += (c[k] >> 16) & 0xff; }
                dst[yy * n + xx] = 0xff000000u | ((b + 2) / 4) << 16 | ((g + 2) / 4) << 8 | ((r + 2) / 4);
            }
        }
    }
    free(px);
    return true;
}

/* the Mode-7 horizon: a dedicated 768x144 panorama (sky_mode7.png) authored to tile cleanly at its own width,
 * unlike level 1's background layers (reused at first) which had dead columns and repeated in narrow, glitchy
 * slices. One full steering turn pans exactly one copy of it by. */
static bool load_sky(Mode7 *m)
{
    const char *png = asset_path("sky_mode7.png");
    if (!png) { fprintf(stderr, "assets/sky_mode7.png missing\n"); return false; }
    int w, h; m->sky_tex = gfx_image_tex(png, &w, &h);
    if (!m->sky_tex) return false;
    rtex_set_scale(m->sky_tex, R_SCALE_NEAREST);
    m->sky_w = w; m->sky_h = h;
    return true;
}

/* draw an atlas sprite, bottom-centred at (x, y_bottom), scaled, optional rotation and colour mod */
static void draw_spr(Mode7 *m, int id, int frame, real cx, real ybot, real scale, real angle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    Spr *s = &m->spr[id]; if (s->frames < 1) return;
    if (frame < 0) frame = 0;
    if (frame >= s->frames) frame = s->frames - 1;
    RFRect src = { r_int(s->x + frame * s->w), r_int(s->y), r_int(s->w), r_int(s->h) };
    real w = s->w * scale, h = s->h * scale;
    RFRect dst = { r_floorr(cx - w / 2), r_floorr(ybot - h), w, h };   /* w / 2: w * 0.5f */
    if (dst.x + w < 0 || dst.x > r_int(m->sw) || dst.y + h < 0 || dst.y > r_int(m->sh)) return;
    rtex_set_color_mod(m->atlas, r, g, b); rtex_set_alpha_mod(m->atlas, a);
    if (angle != 0) r_tex_rot(m->ren, m->atlas, &src, &dst, angle, NULL, R_FLIP_NONE);
    else r_tex(m->ren, m->atlas, &src, &dst);
    rtex_set_color_mod(m->atlas, 255, 255, 255); rtex_set_alpha_mod(m->atlas, 255);
}

/* ---------------------------------------------------------------- worlds */
static void fill_sand(Mode7 *m)
{
    /* frand(m) < 0.5f is the generator's top bit clear: the same draws, without a million float conversions */
    for (int i = 0; i < MAPN * MAPN; i++) { m->rng = m->rng * 1664525u + 1013904223u; m->cells[i] = (m->rng >> 31) ? T_SAND2 : T_SAND; }
    /* scorched patches: a few hundred blobs a few cells across */
    for (int k = 0; k < 400; k++) {
        int cx = r_trunc(frand(m) * MAPN), cy = r_trunc(frand(m) * MAPN), r = 2 + r_trunc(frand(m) * 4);
        for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) if (x * x + y * y <= r * r) cell_set(m, cx + x, cy + y, T_SAND_DARK);
    }
}

/* the New Borderland circuit: a Catmull-Rom loop through hand-placed control points, sampled into TRACK_N points */
static void build_track(Mode7 *m)
{
    /* the start line sits on the long back straight (CP 0), the grid forms up behind it on the same straight */
    static const int CP[][2] = {   /* whole units: the spline's integer sums are exact in either arithmetic */
        { 600, 0 }, { 1500, 0 }, { 2300, 250 }, { 2700, 900 }, { 2400, 1500 }, { 1700, 1800 }, { 1000, 1500 }, { 500, 1950 },
        { -300, 2250 }, { -1200, 1950 }, { -1750, 1250 }, { -1500, 500 }, { -1950, -300 }, { -1350, -950 }, { -700, -650 }, { -300, -80 },
    };
    int n = sizeof CP / sizeof CP[0];
    real ox = r_int(WORLD / 2), oy = r_int(WORLD / 2);
    for (int i = 0; i < TRACK_N; i++) {
        real u = r_int(i) / TRACK_N * n; int k = r_trunc(u); real t = u - r_int(k);
        const int *p0 = CP[(k - 1 + n) % n], *p1 = CP[k % n], *p2 = CP[(k + 1) % n], *p3 = CP[(k + 2) % n];
        real t2 = r_mul(t, t), t3 = r_mul(t2, t);
        real x = (r_int(2 * p1[0]) + r_mul(r_int(-p0[0] + p2[0]), t) + r_mul(r_int(2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]), t2) + r_mul(r_int(-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]), t3)) / 2;   /* / 2: 0.5f * */
        real y = (r_int(2 * p1[1]) + r_mul(r_int(-p0[1] + p2[1]), t) + r_mul(r_int(2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]), t2) + r_mul(r_int(-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]), t3)) / 2;
        m->tx[i] = ox + x; m->ty[i] = oy + y;
    }
    m->tlen[0] = 0;
    for (int i = 1; i <= TRACK_N; i++) {
        int a = i - 1, b = i % TRACK_N;
        real d = r_hypot(m->tx[b] - m->tx[a], m->ty[b] - m->ty[a]);
        if (i < TRACK_N) m->tlen[i] = m->tlen[a] + d; else m->track_len = m->tlen[a] + d;
    }
    /* rasterize: every map cell near the track remembers its nearest sample (squared distance + index), then the
     * distance from the centreline picks the material and the sample's arc length phases the kerb / dash pattern.
     * Only the track's bounding box is searched, with squared distances: the whole map with a hypotf per test took
     * 2.7 s of the stage's load on the Dreamcast (a library call there, about a million of them). */
    fill_sand(m);
    const real HALF = R(104), LINE_W = R(4), KERB = R(118);
    const int RR = r_trunc(KERB / (1 << MAPSH)) + 1;
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
    for (int i = 0; i < TRACK_N; i++) {
        int ccx = r_floor(m->tx[i]) >> MAPSH, ccy = r_floor(m->ty[i]) >> MAPSH;
        bx0 = ccx < bx0 ? ccx : bx0; bx1 = ccx > bx1 ? ccx : bx1; by0 = ccy < by0 ? ccy : by0; by1 = ccy > by1 ? ccy : by1;
    }
    bx0 -= RR; by0 -= RR; bx1 += RR; by1 += RR;
    int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    real *dist = malloc(sizeof(real) * bw * bh); int16_t *near = malloc(sizeof(int16_t) * bw * bh);
    for (int i = 0; i < bw * bh; i++) { dist[i] = R_MAX; near[i] = -1; }   /* squared distances stay under 32768 (the search box) */
    for (int i = 0; i < TRACK_N; i++) {
        int ccx = r_floor(m->tx[i]) >> MAPSH, ccy = r_floor(m->ty[i]) >> MAPSH;
        for (int cy = ccy - RR; cy <= ccy + RR; cy++) {
            real dy = r_int(cy << MAPSH) + r_int(1 << MAPSH) / 2 - m->ty[i], dy2 = r_mul(dy, dy);
            real *drow = dist + (cy - by0) * bw - bx0; int16_t *nrow = near + (cy - by0) * bw - bx0;
            for (int cx = ccx - RR; cx <= ccx + RR; cx++) {
                real dx = r_int(cx << MAPSH) + r_int(1 << MAPSH) / 2 - m->tx[i], d2 = r_mul(dx, dx) + dy2;
                if (d2 < drow[cx]) { drow[cx] = d2; nrow[cx] = (int16_t)i; }
            }
        }
    }
    for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
        int k = y * bw + x; real d2 = dist[k]; if (d2 >= r_mul(KERB, KERB)) continue;
        real along = m->tlen[near[k]];
        uint8_t t;
        if (along < R(28)) t = d2 < r_mul(HALF, HALF) ? T_CHECKER : T_SAND;                            /* start / finish line */
        else if (d2 < R(3 * 3)) t = (r_trunc(along / 48) & 1) ? T_DASH : T_ASPHALT;                 /* centre dashes */
        else if (d2 < r_mul(HALF - LINE_W, HALF - LINE_W)) t = T_ASPHALT;
        else if (d2 < r_mul(HALF, HALF)) t = T_LINE;
        else t = (r_trunc(along / 40) & 1) ? T_KERB_RED : T_KERB_WHITE;
        cell_set(m, bx0 + x, by0 + y, t);
    }
    free(dist); free(near);
    if (plat_getenv("SABER_M7MAP")) {   /* debug: dump the material map */
        FILE *f = fopen(plat_getenv("SABER_M7MAP"), "wb");
        if (f) { fprintf(f, "P5\n%d %d\n255\n", MAPN, MAPN); for (int i = 0; i < MAPN * MAPN; i++) fputc(m->cells[i] * 20, f); fclose(f); }
    }
}

/* nearest track sample to (x,y) starting the search from a hint; returns s, lateral (signed, +right of travel) */
static int track_project(const Mode7 *m, real x, real y, int hint, real *s_out, real *lat_out, real *tang_out)
{
    int best = hint; rdist2 bd = DIST2_MAX;
    int span = hint < 0 ? TRACK_N : 40;
    for (int k = -span; k <= span; k++) {
        int i = ((hint < 0 ? 0 : hint) + k + TRACK_N * 4) % TRACK_N;
        rdist2 d = dist2(dwrap(x, m->tx[i]), dwrap(y, m->ty[i]));
        if (d < bd) { bd = d; best = i; }
        if (hint < 0 && k >= TRACK_N - 1) break;
    }
    int nx = (best + 1) % TRACK_N;
    real tx = m->tx[nx] - m->tx[best], ty = m->ty[nx] - m->ty[best]; real tl = r_hypot(tx, ty); tx = r_div(tx, tl); ty = r_div(ty, tl);
    real rx = dwrap(x, m->tx[best]), ry = dwrap(y, m->ty[best]);
    real along = r_mul(rx, tx) + r_mul(ry, ty);
    if (s_out) { *s_out = m->tlen[best] + along; if (*s_out < 0) *s_out += m->track_len; if (*s_out >= m->track_len) *s_out -= m->track_len; }
    if (lat_out) *lat_out = r_mul(rx, -ty) + r_mul(ry, tx);   /* right of travel: (-ty, tx) */
    if (tang_out) *tang_out = r_atan2(ty, tx);
    return best;
}
static void track_point(const Mode7 *m, real s, real lat, real *x, real *y, real *heading)
{
    s = r_fmod(s, m->track_len); if (s < 0) s += m->track_len;
    int i = r_trunc(r_div(s, m->track_len) * TRACK_N) % TRACK_N;   /* samples are nearly uniform */
    while (i > 0 && m->tlen[i] > s) i--;
    while (i < TRACK_N - 1 && m->tlen[i + 1] <= s) i++;
    int n = (i + 1) % TRACK_N;
    real seg = (n ? m->tlen[n] : m->track_len) - m->tlen[i]; real t = seg > 0 ? r_div(s - m->tlen[i], seg) : 0;
    real tx = m->tx[n] - m->tx[i], ty = m->ty[n] - m->ty[i]; real tl = r_hypot(tx, ty); tx = r_div(tx, tl); ty = r_div(ty, tl);
    *x = m->tx[i] + r_mul(r_mul(tx, seg), t) + r_mul(-ty, lat); *y = m->ty[i] + r_mul(r_mul(ty, seg), t) + r_mul(tx, lat);
    if (heading) *heading = r_atan2(ty, tx);
}

static void place_props_around_track(Mode7 *m)
{
    for (int i = 0; i < 90; i++) {
        real s = r_mul(frand(m), m->track_len), side = frand(m) < R(0.5f) ? R(-1) : R(1), lat = r_mul(side, R(170) + frand(m) * 400);
        real x, y; track_point(m, s, lat, &x, &y, NULL);
        if (is_road(cell_at(m, x, y)) || is_rumble(cell_at(m, x, y))) continue;
        Ent *e = ent_new(m); if (!e) break;
        real r = frand(m);
        e->kind = K_PROP; e->spr = r < R(0.45f) ? S_CACTUS : r < R(0.8f) ? S_ROCK_S : r < R(0.95f) ? S_ROCK_B : S_MESA; e->x = wrapf(x); e->y = wrapf(y);
        e->scale = e->spr == S_MESA ? R(2.2f) : e->spr == S_ROCK_B ? R(1.5f) : R(1.2f); e->solid = true;
    }
    /* the finish gate */
    Ent *g = ent_new(m);
    if (g) { g->kind = K_GATE; g->spr = S_GATE; g->x = m->tx[0]; g->y = m->ty[0]; g->scale = R(3.9f); }   /* 160 px x 3.9 x CAM_BACK / FOCAL = the road's 208 units plus the kerbs */
}

/* the open desert with the dirt road north to Dome City down its middle (x = WORLD / 2), soft shoulders */
static void build_desert(Mode7 *m)
{
    fill_sand(m);
    const int ROAD = 108 >> MAPSH, SHOULDER = 128 >> MAPSH, mid = MAPN / 2;
    for (int cy = 0; cy < MAPN; cy++) {
        int wob = r_trunc(r_sin(cy * R(0.021f)) * 3 + r_sin(cy * R(0.0071f)) * 5);   /* the road meanders a little */
        for (int cx = mid + wob - SHOULDER; cx <= mid + wob + SHOULDER; cx++) cell_set(m, cx, cy, abs(cx - mid - wob) <= ROAD ? T_DIRT : T_SHOULDER);
    }
}

/* ---------------------------------------------------------------- stage setup */
static const char *const SCRIPT_INTRO =
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nNew Borderland... the All Galaxy Grand Prix. I haven't sat on a grid like this since Cavalry Command recruited me.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nEnjoy it, Fireball. It's Marco Firenza's last race, and the three of us are riding along - give us a good show.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nKeep your eyes open. That Black Hornets team came out of nowhere and nobody has seen their faces.\n<<>>\n"
    "<|PURPLE|>\n</dialog_avatar_outrider/>\nFireball! I'm Claudia - your biggest fan! Meet me behind the paddock after qualifying? Alone?\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\n...Sure. Hey, what's- OUTRIDERS! It's a trap!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nFireball, get DOWN! ...You owe me one, hotshot. Now get back in that car - the race is about to start.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nThose Hornets are Outriders in disguise, pardner. Whatever they came for, it isn't the trophy.\n<<>>\n";
static const char *const SCRIPT_BREAKAWAY =
    "<|RED|>\n</dialog_avatar_april2/>\nThe Black Hornets are leaving the course! They're heading for Dome City - the Cavalry Command Nerve Center!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nFirenza can keep his trophy. Hang on, everybody - I'm going after them!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nRun their leader down before he reaches the dome, Fireball. Colt and I will cover you from the back seat.\n<<>>\n";
static const char *const SCRIPT_CAUGHT =
    "<|PURPLE|>\nYou again, Star Sheriff! Nobody catches the Hornets on the open road. Eat my mines - and my dust!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nNobody outruns the Red Fury, bug. Ram him off that road! April, Colt - light him up!\n<<>>\n";
static const char *const SCRIPT_VICTORY =
    "<|GREEN|>\n</dialog_avatar_saber2/>\nThat's the last of the Black Hornets. Dome City never even knew, Fireball.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nClaudia was a Vapor Zone puppet all along... don't take it personally, champ.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nMarco Firenza wins his last Grand Prix - and I'll take that rematch any day. Let's go home, Ramrod.\n<<>>\n";

static void begin_pursuit(Mode7 *m);
static void begin_boss(Mode7 *m);

static void start_race(Mode7 *m)
{
    ents_clear(m, false);
    build_track(m);
    if (m->floor) r_floor_cells_changed(m->floor);
    place_props_around_track(m);
    /* the grid: 8 cars, two abreast, behind the line */
    static const struct { int spr; real max; bool hornet; const char *name; } FIELD[N_RACERS] = {
        { S_FIRENZA, R(575), false, "FIRENZA" }, { S_HORNET, R(555), true, "HORNET" }, { S_HORNET, R(540), true, "HORNET" },
        { S_RBLUE, R(525), false, "VEGA" }, { S_HORNET, R(530), true, "HORNET" }, { S_RPURPLE, R(510), false, "KELLY" }, { S_RBLUE, R(500), false, "DUNN" },
    };
    for (int i = 0; i < N_RACERS; i++) {
        Ent *e = ent_new(m); if (!e) break;
        e->kind = K_RACER; e->spr = FIELD[i].spr; e->max_speed = FIELD[i].max; e->hornet = FIELD[i].hornet; e->id = i;
        e->s = m->track_len - R(60.0f) * (i + 1) - R(30); e->lat = (i & 1) ? R(48) : R(-48); e->lat_target = e->lat; e->solid = true;
        e->lap = -1;   /* the grid sits behind the line: progress (lap * track_len + s) is the distance past it, like the player's */
        e->hp = e->hp_max = r_int((e->hornet ? 10 : 7) + 2 * m->difficulty);
        track_point(m, e->s, e->lat, &e->x, &e->y, &e->heading); e->speed = 0;
    }
    /* player last on the grid (right column) */
    m->s = m->track_len - R(60.0f) * (N_RACERS + 1) - R(30); m->lat = R(48); m->lap = 0; m->progress = PROG(-1, m->s);   /* behind the line: the first crossing is lap 1 */
    track_point(m, m->s, m->lat, &m->px, &m->py, &m->heading); m->cam_heading = m->heading; m->speed = 0; m->last_s = m->s;
    m->near_idx = -1; m->race_time = 0;
}

Mode7 *mode7_create(Ren *ren, int sw, int sh, int difficulty, int lives, bool resume_phase2)
{
    Mode7 *m = calloc(1, sizeof *m);
    m->ren = ren; m->sw = sw; m->sh = sh; m->rng = 0xC0FFEE;
    m->floor_h = sh - HORIZON - 1;
    m->ok = load_atlas(m);
    sfx_preload_file(asset_path("sfx/turbo_start.wav")); sfx_preload_loop(asset_path("sfx/turbo_loop.wav"));
    if (m->ok && m->spr[S_BUGGY].frames < 5) { fprintf(stderr, "mode7.png is stale (buggy needs 5 steering frames): rerun tools/build_mode7_assets.py\n"); m->ok = false; }
    m->sky_ok = load_sky(m);
    m->difficulty = difficulty; m->lives = lives;
    m->max_hp = difficulty == 0 ? 16 : difficulty == 1 ? 12 : 8; m->hp = m->max_hp;   /* the car's damage meter: shots 1, mines / crashes 2 */
    m->boost = R(1.0f); m->boost_locked = false; m->music_now = -1;
    dialog_set_hero(HERO_FIREBALL);   /* the Grand Prix is Fireball's story whoever was picked: everyone rides in his buggy */
    start_race(m);
    m->phase = PH_INTRO; m->phase_t = 0;
    m->intro_pending = true;
    if (plat_getenv("SABER_M7PHASE")) {   /* debug: 1 race (no story), 2 pursuit, 3 boss, 4 the finish -> briefing */
        int ph = atoi(plat_getenv("SABER_M7PHASE")); m->intro_pending = false;
        if (ph == 1) { m->phase = PH_COUNTDOWN; m->countdown = R(3.99f); if (plat_getenv("SABER_M7LAP")) { m->lap = 1; m->progress = PROG(1, m->s); for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_RACER) m->ents[i].lap = 1; } }
        else if (ph == 2) { m->phase = PH_PURSUIT; begin_pursuit(m); }
        else if (ph == 3) { begin_pursuit(m); m->phase = PH_BOSS; begin_boss(m); if (plat_getenv("SABER_M7BOSSHP")) m->ents[m->boss_i].hp = r_parse(plat_getenv("SABER_M7BOSSHP"), NULL); }
        else if (ph == 4) { m->phase = PH_RACE; m->lap = 2; m->progress = PROG(2, m->s); m->last_s = m->s; m->speed = R(500); for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_RACER) { m->ents[i].lap = 2; m->ents[i].speed = R(500); } }   /* the grid, two laps in: 510 units to the flag */
    }
    if (resume_phase2) { m->intro_pending = false; m->phase = PH_PURSUIT; m->phase_t = 0; begin_pursuit(m); }
    return m;
}

void mode7_destroy(Mode7 *m)
{
    if (!m) return;
    sfx_loop(NULL);
    if (m->atlas) rtex_destroy(m->atlas);
    if (m->floor) r_floor_destroy(m->floor);
    if (m->sky_tex) rtex_destroy(m->sky_tex);
    free(m);
}

int mode7_result(const Mode7 *m) { return m->result; }
int mode7_lives(const Mode7 *m) { return m->lives; }
bool mode7_phase2_reached(const Mode7 *m)
{
    int phase = m->phase == PH_DEAD || m->phase == PH_GAMEOVER ? m->resume_phase : m->phase;
    return phase == PH_PURSUIT || phase == PH_BOSS || phase == PH_VICTORY || phase == PH_CLEARED;
}

/* ---------------------------------------------------------------- combat helpers */
static bool phase_plays(const Mode7 *m) { return m->phase == PH_RACE || m->phase == PH_PURSUIT || m->phase == PH_BOSS; }

static void player_hurt(Mode7 *m, int dmg)
{
    if (m->hurt_t > 0 || !phase_plays(m)) return;   /* no damage while a story scene or the countdown holds the car */
    m->hp -= dmg; m->hurt_t = R(0.7f); m->shake = R(0.4f); sfx_play(3, 0);
    if (m->hp <= 0) {
        m->hp = 0; spawn_expl(m, m->px, m->py, R(1.6f)); sfx_play(5, 0); sfx_play(6, 3); sfx_loop(NULL); m->turbo_on = false;
        m->phase_t = 0; m->dead_t = 0; m->resume_phase = m->phase;
        m->phase = PH_DEAD; m->speed = 0;
    }
}

static void fire_shot(Mode7 *m, real x, real y, real heading, real speed, bool player_owned, real z)
{
    Ent *e = ent_new(m); if (!e) return;
    e->kind = player_owned ? K_SHOT : K_ESHOT; e->spr = player_owned ? S_SHOT : S_ESHOT;
    e->x = x; e->y = y; e->z = z; e->heading = heading; e->speed = speed; e->player_owned = player_owned; e->t = player_owned ? R(1.1f) : R(2.2f);
    e->vx = r_mul(r_cos(heading), speed); e->vy = r_mul(r_sin(heading), speed);
    if (player_owned) { Ent *f = ent_new(m); if (f) { f->kind = K_FLASH; f->spr = S_FLASH; f->x = x; f->y = y; f->z = z; f->t = R(0.08f); } }
}

/* enemy guns are deliberately sloppy: a shot goes at the player with this much random spread (radians) */
static real aim_at_player(Mode7 *m, const Ent *e, real spread) { return r_atan2(dwrap(m->py, e->y), dwrap(m->px, e->x)) + r_mul(frand(m) - R(0.5f), spread); }

static void damage_ent(Mode7 *m, Ent *e, real dmg)
{
    if (e->kind == K_BOSS && e->state == B_DYING) return;
    e->hp -= dmg; e->knock = R(0.25f); sfx_play(14, 0);
    if (e->hp <= 0) {
        if (e->kind == K_BOSS) {   /* the leader burns out over a couple of seconds before he blows (update_ents) */
            e->state = B_DYING; e->t = R(2.4f); e->t2 = 0; e->hp = 0; m->kills++;
            spawn_expl(m, e->x, e->y, R(1.2f)); sfx_play(5, 0); set_msg(m, "HORNET LEADER DOWN", R(2.0f));
            return;
        }
        spawn_expl(m, e->x, e->y, R(1.2f));
        sfx_play(5, 0); sfx_play(6, 3); m->kills++;
        if (e->kind == K_RACER || e->kind == K_ESCORT) set_msg(m, e->hornet ? "BLACK HORNET DESTROYED" : "RIVAL WRECKED", R(2.0f));
        e->kind = K_NONE;
    }
}

/* the leader's Hornet escort: runs north ahead of the player and drops back to block and shoot */
static void spawn_escorts(Mode7 *m, int n)
{
    for (int k = 0; k < n; k++) {
        Ent *e = ent_new(m); if (!e) break;
        e->kind = K_ESCORT; e->spr = S_HORNET; e->hornet = true; e->hp = e->hp_max = r_int(4 + m->difficulty); e->scale = R(1.0f); e->solid = true;
        e->x = wrapf(r_int(WORLD / 2) + (frand(m) - R(0.5f)) * 160); e->y = wrapf(m->py - R(900) - frand(m) * 300); e->state = 0; e->t = R(1.0f) + frand(m) * 2; e->heading = -PI / 2; e->speed = R(300);
    }
}

/* ---------------------------------------------------------------- update: the player's car */
/* a hit spins the buggy once round; a second hit mid-spin restarts the turn but keeps the original heading */
static void start_spin(Mode7 *m, real dur)
{
    if (m->spin_dur <= 0) m->spin_heading = m->heading;
    m->spin_t = m->spin_dur = dur;
}

static void player_drive(Mode7 *m, const Input *in, real dt, bool free_drive)
{
    uint8_t tile = cell_at(m, m->px, m->py);
    bool road = is_road(tile), rumble = is_rumble(tile);
    real vmax = road ? R(470.0f) : rumble ? R(380.0f) : R(250.0f);
    if (m->boost_locked && m->boost >= R(0.5f)) m->boost_locked = false;   /* recharged halfway: turbo usable again */
    bool turbo = !m->boost_locked && btn_down(in, BTN_AIM) && m->boost > R(0.05f) && m->spin_t <= 0 && phase_plays(m);
    if (!free_drive && plat_getenv("SABER_M7AUTO") && atoi(plat_getenv("SABER_M7AUTO")) >= 2 && m->spin_t <= 0 && phase_plays(m))   /* debug: the auto-driver also uses the turbo */
        turbo = !m->boost_locked && (m->turbo_on ? m->boost > R(0.05f) : m->boost > R(0.6f));
    if (turbo) {
        vmax = r_mul(vmax, R(1.35f)); m->boost -= r_mul(dt, R(0.33f));
        /* used up: locked out until halfway. The floor is 0.05 (the engage gate below), not 0: otherwise a held
         * button hovers just under 0.05, flickering turbo without ever locking */
        if (m->boost <= R(0.05f)) { m->boost = 0; m->boost_locked = true; }
    }
    else { m->boost = clampf(m->boost + r_mul(dt, R(0.08f)), 0, R(1)); if (m->boost_locked && m->boost >= R(0.5f)) m->boost_locked = false; }
    if (turbo && !m->turbo_on) { sfx_play_file(asset_path("sfx/turbo_start.wav")); sfx_loop(asset_path("sfx/turbo_loop.wav")); }
    else if (!turbo && m->turbo_on) sfx_loop(NULL);
    m->turbo_on = turbo; if (turbo) m->turbo_t += dt;
    /* slipstream: tucked in close behind a rival, the car tows along a little faster */
    if (!free_drive && !turbo) for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &m->ents[i]; if (e->kind != K_RACER) continue;
        real gap = PROG_GAP(e);
        if (gap > R(20) && gap < R(260) && r_abs(e->lat - m->lat) < R(40)) { vmax = r_mul(vmax, R(1.12f)); break; }
    }
    bool accel = btn_down(in, BTN_JUMP) || btn_down(in, BTN_UP);
    bool brake = btn_down(in, BTN_DOWN);
    real steer = r_int((btn_down(in, BTN_LEFT) ? -1 : 0) + (btn_down(in, BTN_RIGHT) ? 1 : 0));
    if (!free_drive && plat_getenv("SABER_M7AUTO")) {   /* debug: drive along the circuit (test laps without a driver) */
        real ax, ay; track_point(m, m->s + R(260), 0, &ax, &ay, NULL);
        real want = r_atan2(dwrap(ay, m->py), dwrap(ax, m->px)), d = angdiff(want, m->heading);
        steer = d > R(0.05f) ? R(1) : d < R(-0.05f) ? R(-1) : 0; accel = true;
    }
    if (m->spin_t > 0) { accel = false; m->spin_t -= dt; if (m->spin_t < 0) m->spin_t = 0; }
    if (accel) m->speed += r_mul_dt(turbo ? R(520.0f) : R(300.0f), dt);
    else m->speed -= r_mul_dt(R(120.0f), dt);
    if (brake) m->speed -= r_mul_dt(R(500.0f), dt);
    if (m->speed > vmax) m->speed -= r_mul_dt(r_mul(m->speed - vmax, road ? R(6.0f) : R(8.0f)), dt);
    if (m->speed < 0) m->speed = brake ? clampf(m->speed, R(-80), 0) : 0;
    if (m->spin_t > 0) steer = 0;
    real turn = r_mul(r_mul(R(1.9f), steer), clampf(r_div(r_abs(m->speed), R(300.0f)), 0, R(1.15f)));
    if (!road && !rumble) turn = r_mul(turn, R(0.85f));
    m->heading += r_mul_dt(turn, dt);
    m->tilt += r_mul(r_mul(steer, R(7.0f)) - m->tilt, clampf(dt * 8, 0, R(1)));
    if (m->spin_dur > 0) {   /* a spin-out: the car fishtails and settles back on the heading it had when hit */
        real p = R(1) - r_div(m->spin_t, m->spin_dur);
        m->heading = m->spin_heading + r_mul(r_mul(R(0.45f), r_sin(r_mul(TWO_PI, p))), R(1) - p);
        if (m->spin_t <= 0) { m->heading = m->spin_heading; m->spin_dur = 0; }
    }
    m->vx = r_mul(r_cos(m->heading), m->speed); m->vy = r_mul(r_sin(m->heading), m->speed);
    m->px = wrapf(m->px + r_mul_dt(m->vx, dt)); m->py = wrapf(m->py + r_mul_dt(m->vy, dt));
    /* camera heading lags a touch behind the car for the drifting feel */
    m->cam_heading += r_mul(angdiff(m->heading, m->cam_heading), clampf(dt * 9, 0, R(1)));
    m->anim_t += r_mul(r_mul_dt(r_abs(m->speed), dt), R(0.02f));
    if (rumble) m->bounce = R(1.5f) * (r_trunc(m->anim_t * 8) & 1); else m->bounce = (!road && m->speed > R(100)) ? r_int(r_trunc(m->anim_t * 6) & 1) : 0;
    if (m->fire_cd > 0) m->fire_cd -= dt;
    if (btn_down(in, BTN_SHOOT) && m->fire_cd <= 0 && m->spin_t <= 0 && !m->fire_hold) {
        m->fire_cd = R(0.16f); sfx_play(1, 0);
        fire_shot(m, m->px + r_cos(m->heading) * 20, m->py + r_sin(m->heading) * 20, m->heading, R(1500.0f) + r_abs(m->speed), true, R(14));
    }
    if (!free_drive) {
        real s; m->near_idx = track_project(m, m->px, m->py, m->near_idx, &s, &m->lat, NULL);
        real ds = s - m->last_s;
        if (ds < -m->track_len / 2) { ds += m->track_len; }   /* / 2: * 0.5f */
        else if (ds > m->track_len / 2) { ds -= m->track_len; }
        m->progress += ds; m->last_s = s; m->s = s;
        int lap = prog_lap(m->progress, m->track_len); if (lap < 0) lap = 0;   /* a lap counts under the gate (s = 0), not a track length after the grid */
        if (lap > m->lap) { m->lap = lap; set_msg(m, lap == 1 ? "LAP 2" : lap == 2 ? "FINAL LAP" : "FINISH", R(2.0f)); }
    }
}

/* ---------------------------------------------------------------- update: entities */
static void update_racers(Mode7 *m, real dt)
{
    /* a light rubber band around the player's progress: the pack stays reachable, but a leader is only reeled in
     * with turbo and the slipstream (the player settles at ~520 on the road, ~720 on turbo) */
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &m->ents[i]; if (e->kind != K_RACER) continue;
        real gap = PROG_GAP(e);
        real target = e->max_speed;
        if (gap > R(2600)) target = r_mul(target, R(0.75f)); else if (gap > R(1200)) target = r_mul(target, R(0.86f)); else if (gap < R(-900)) target = r_mul(target, R(1.12f));
        if (e->knock > 0) { e->knock -= dt; target = r_mul(target, R(0.5f)); }
        if (m->phase == PH_COUNTDOWN || m->phase == PH_INTRO || m->phase == PH_INSTRUCTIONS) target = 0;
        if (m->phase == PH_FINISH || m->phase == PH_BREAKAWAY) target = e->hornet ? R(760) : r_mul(target, R(0.8f));   /* the Hornets bolt, the field winds down */
        e->speed += r_mul(target - e->speed, clampf(r_mul(dt, e->speed < target ? R(1.6f) : R(2.5f)), 0, R(1)));
        /* racing line wobble; a car just ahead of the player drifts over to block the pass. The grid holds still
         * (no line changes, no steering pose) until the lights go out */
        bool grid = m->phase == PH_COUNTDOWN || m->phase == PH_INTRO || m->phase == PH_INSTRUCTIONS;
        if (!grid) {
            e->t -= dt;
            if (e->t <= 0) { e->t = R(1.5f) + frand(m) * 3; e->lat_target = (frand(m) - R(0.5f)) * 150; }
            if (gap > R(30) && gap < R(320) && m->phase == PH_RACE) e->lat_target += r_mul(m->lat - e->lat_target, clampf(r_mul(dt, e->hornet ? R(1.6f) : R(0.8f)), 0, R(1)));
            e->lat += r_mul(e->lat_target - e->lat, clampf(r_mul(dt, R(1.2f)), 0, R(1)));
        } else { e->lat_target = e->lat; e->tilt = 0; }
        real prev_s = e->s; e->s += r_mul_dt(e->speed, dt);
        if (e->s >= m->track_len) { e->s -= m->track_len; e->lap++; }
        (void)prev_s;
        track_point(m, e->s, e->lat, &e->x, &e->y, &e->heading);
        e->tilt += r_mul(r_mul(e->lat_target - e->lat, R(0.05f)) - e->tilt, clampf(dt * 5, 0, R(1)));   /* steering pose from where it is heading */
        /* Black Hornets fight: mines behind when ahead of the player, shots when behind */
        if (e->hornet && m->phase == PH_RACE && m->race_time > R(8.0f) && e->speed > R(200)) {
            e->t2 -= dt;
            if (e->t2 <= 0) {
                if (gap > R(60) && gap < R(900)) { e->t2 = R(1.8f) + r_mul(frand(m), R(1.5f)); Ent *mn = ent_new(m); if (mn) { mn->kind = K_MINE; mn->spr = S_MINE; mn->x = e->x; mn->y = e->y; mn->t = R(12); } }
                else if (gap < R(-40) && gap > R(-800)) { e->t2 = R(1.2f) + frand(m); fire_shot(m, e->x, e->y, aim_at_player(m, e, R(0.5f)), R(650), false, R(12)); sfx_play(7, 0); }
                else e->t2 = R(0.5f);
            }
        }
    }
}

static void bump_player(Mode7 *m, Ent *e, real dist, real r)
{
    real nx = dwrap(m->px, e->x), ny = dwrap(m->py, e->y); if (dist < R(1)) { nx = R(1); ny = 0; dist = R(1); }
    nx = r_div(nx, dist); ny = r_div(ny, dist); real push = r_mul(r - dist, R(0.6f));
    m->px = wrapf(m->px + r_mul(nx, push)); m->py = wrapf(m->py + r_mul(ny, push));
    if (e->kind == K_RACER) { e->lat += r_mul((e->lat > m->lat ? 1 : -1) * push, R(0.6f)); e->speed = r_mul(e->speed, R(0.9f)); m->speed = r_mul(m->speed, R(0.9f)); m->shake = R(0.15f); }
}

static void update_ents(Mode7 *m, real dt)
{
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &m->ents[i]; if (e->kind == K_NONE) continue;
        switch (e->kind) {
        case K_SHOT: case K_ESHOT:
            e->x = wrapf(e->x + r_mul_dt(e->vx, dt)); e->y = wrapf(e->y + r_mul_dt(e->vy, dt)); e->t -= dt;
            if (e->t <= 0) { e->kind = K_NONE; break; }
            if (e->kind == K_SHOT) {
                for (int j = 0; j < MAX_ENT; j++) {
                    Ent *o = &m->ents[j];
                    if (o->kind != K_RACER && o->kind != K_ESCORT && o->kind != K_BOSS && o->kind != K_MINE) continue;
                    real r = o->kind == K_BOSS ? R(42) : o->kind == K_MINE ? R(18) : R(34);
                    if (r_abs(dwrap(e->x, o->x)) < r && r_abs(dwrap(e->y, o->y)) < r) {
                        e->kind = K_NONE;
                        if (o->kind == K_MINE) { spawn_expl(m, o->x, o->y, R(0.8f)); o->kind = K_NONE; sfx_play(5, 0); }
                        else if (o->kind == K_BOSS && o->state == B_FLEE) { o->knock = R(0.3f); sfx_play(14, 0); }   /* a hit slows the fleeing leader: shooting helps close the gap */
                        else damage_ent(m, o, R(1));
                        break;
                    }
                }
            } else if (r_abs(dwrap(e->x, m->px)) < R(22) && r_abs(dwrap(e->y, m->py)) < R(22)) { e->kind = K_NONE; player_hurt(m, 1); }   /* a hit on the car */
            break;
        case K_FLASH: e->t -= dt; if (e->t <= 0) e->kind = K_NONE; break;
        case K_EXPL: e->t += dt; e->frame = r_trunc(e->t * 14); if (e->frame >= 6) e->kind = K_NONE; if (e->frame == 2 && e->t2 == 0) { e->t2 = R(1); spawn_smoke(m, e->x, e->y, 20 * e->scale); } break;
        case K_SMOKE: e->t += dt; e->frame = r_trunc(e->t * 6); e->z += r_mul_dt(R(30), dt); if (e->frame >= 4) e->kind = K_NONE; break;
        case K_MINE:
            e->t -= dt; e->anim += dt; e->frame = r_trunc(e->anim * 4) & 1;
            if (e->t <= 0) { e->kind = K_NONE; break; }
            if (r_abs(dwrap(e->x, m->px)) < R(26) && r_abs(dwrap(e->y, m->py)) < R(26)) {
                spawn_expl(m, e->x, e->y, R(1.0f)); e->kind = K_NONE;
                player_hurt(m, 2);
                if (m->phase != PH_DEAD) { m->speed = r_mul(m->speed, R(0.4f)); start_spin(m, R(0.6f)); }
            }
            break;
        case K_PROP: {
            real dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y); real r = e->spr == S_CACTUS ? R(18) : e->spr == S_MESA ? R(70) : e->spr == S_ROCK_B ? R(40) : R(24);
            real d = r_hypot(dx, dy);
            if (d < r + R(16)) {
                real nx = r_div(dx, d > R(1) ? d : R(1)), ny = r_div(dy, d > R(1) ? d : R(1));
                m->px = wrapf(e->x + r_mul(nx, r + R(17))); m->py = wrapf(e->y + r_mul(ny, r + R(17)));
                if (m->speed > R(150) && m->hurt_t <= 0) { player_hurt(m, 2); start_spin(m, R(0.5f)); }
                m->speed = r_mul(-r_abs(m->speed), R(0.35f)) - R(40); m->shake = R(0.2f);   /* bounce off */
            }
            break; }
        case K_RACER: {
            real dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y); real d = r_hypot(dx, dy);
            if (d < R(44)) bump_player(m, e, d, R(44));
            break; }
        case K_ESCORT: {
            /* the leader's Hornet escort: runs north ahead of the player, weaving, and drops back to block and shoot */
            e->t -= dt;
            real dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y), d = r_hypot(dx, dy);
            real prev_h = e->heading;
            if (e->state == 0) {   /* running, weaving across the road */
                e->heading = -PI / 2 + r_mul(r_sin(e->anim), R(0.35f)); e->anim += r_mul(dt, R(1.3f));
                e->speed += r_mul(R(300) - e->speed, clampf(dt * 2, 0, R(1)));
                if (e->t <= 0) { e->state = 1; e->t = R(2.5f) + r_mul(frand(m), R(1.5f)); }
            } else {               /* slowed right down, guns on the player */
                e->speed += r_mul(R(90) - e->speed, clampf(dt * 3, 0, R(1))); e->t2 -= dt;
                e->heading = -PI / 2 + r_mul(r_sin(e->anim * 3), R(0.1f));
                if (e->t2 <= 0 && d < R(1400)) { e->t2 = R(1.1f); fire_shot(m, e->x, e->y, aim_at_player(m, e, R(0.45f)), R(600), false, R(12)); sfx_play(7, 0); }
                if (e->t <= 0) { e->state = 0; e->t = R(3) + frand(m) * 3; }
            }
            e->tilt += r_mul(angdiff(e->heading, prev_h) * 60 - e->tilt, clampf(dt * 6, 0, R(1)));
            if (e->knock > 0) { e->knock -= dt; e->speed = r_mul(e->speed, R(0.97f)); }
            e->x = wrapf(e->x + r_mul_dt(r_mul(r_cos(e->heading), e->speed), dt)); e->y = wrapf(e->y + r_mul_dt(r_mul(r_sin(e->heading), e->speed), dt));
            if (d < R(44)) { bump_player(m, e, d, R(44)); if (m->hurt_t <= 0) player_hurt(m, 1); m->speed = r_mul(m->speed, R(0.6f)); }
            /* fell too far behind the player (he passed it): drop it */
            if (dy < R(-1800) || dy > R(5000)) e->kind = K_NONE;
            break; }
        case K_BOSS: {
            e->t -= dt; if (e->knock > 0) e->knock -= dt;
            real dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y), d = r_hypot(dx, dy);
            real prev_h = e->heading;
            real gap = dy;   /* how far north of the player he is (dy = player y - his y, y grows southward) */
            if (e->state == B_DYING) {   /* burning out: rolls to a stop, sparks and smoke, then the big one */
                e->speed += r_mul(0 - e->speed, clampf(r_mul(dt, R(1.2f)), 0, R(1)));
                e->t2 -= dt;
                if (e->t2 <= 0) { e->t2 = R(0.16f); spawn_expl(m, e->x + (frand(m) - R(0.5f)) * 50, e->y + (frand(m) - R(0.5f)) * 50, R(0.6f) + r_mul(frand(m), R(0.5f))); spawn_smoke(m, e->x, e->y, R(30)); if (frand(m) < R(0.5f)) sfx_play(14, 0); }
                e->heading += r_mul_dt(r_mul(r_sin(e->t * 9), R(0.6f)), dt);   /* fishtailing */
                if (e->t <= 0) {
                    spawn_expl(m, e->x, e->y, R(2.6f)); spawn_expl(m, e->x + R(30), e->y - R(20), R(1.6f)); spawn_expl(m, e->x - R(30), e->y + R(20), R(1.6f));
                    sfx_play(5, 0); sfx_play(6, 3); sfx_play(14, 6); m->shake = R(0.6f);
                    m->wreck_x = e->x; m->wreck_y = e->y; m->wreck_t = 0;
                    e->kind = K_NONE; break;
                }
            } else if (e->state == B_FLEE) {   /* the pursuit: up the road, weaving, pace rubber-banded to the gap so he stays in reach but never free */
                real target = gap > R(3400) ? R(250) : gap > R(2200) ? R(380) : gap < R(600) ? R(540) : R(470);   /* the player does ~550 on the road, ~700 on turbo */
                if (e->knock > 0 && e->t <= 0) target = r_mul(target, R(0.75f));
                if (e->t <= 0 && gap < R(560) && gap > 0 && m->pursuit_t < R(40) && e->t < R(-3)) {   /* early in the chase he always has one more booster (e->t: > 0 boosting, < 0 seconds since) */
                    e->t = R(3.5f); e->speed = R(700); set_msg(m, "THE HORNET HITS HIS BOOSTER", R(1.5f)); sfx_play(0x13, 0);
                    for (int k = 0; k < 3; k++) spawn_smoke(m, e->x - r_cos(e->heading) * 30 * k, e->y - r_sin(e->heading) * 30 * k, R(10));
                }
                if (e->t > 0) target = R(700);   /* booster */
                e->speed += r_mul(target - e->speed, clampf(r_mul(dt, e->t > 0 ? R(4.0f) : R(1.5f)), 0, R(1)));
                e->anim += r_mul(dt, R(0.9f));
                real road_x = r_int(WORLD / 2) + r_sin(e->anim) * 60;
                e->heading = -PI / 2 + clampf(r_mul(dwrap(road_x, e->x), R(0.004f)), R(-0.4f), R(0.4f));
                e->t2 -= dt;
                if (e->t2 <= 0 && gap < R(900) && gap > 0) {   /* mines out the back when the player is close */
                    e->t2 = R(1.4f) + frand(m); Ent *mn = ent_new(m); if (mn) { mn->kind = K_MINE; mn->spr = S_MINE; mn->x = e->x; mn->y = e->y; mn->t = R(14); }
                }
            } else {   /* B_RUN, the fight (Chase H.Q.): he keeps racing up the road just ahead of you, weaving to block, mines out the
                        * back, a rear gunner, a booster now and then; you shoot him and ram him. Pace rubber-banded to the gap */
                real target = gap < 0 ? R(560) : gap < R(140) ? R(600) : gap < R(520) ? R(445) : gap < R(1200) ? R(360) : R(280);   /* the player does ~470 on the dirt, ~630 on turbo; passed, he re-passes */
                if (e->knock > 0) target = r_mul(target, R(0.8f));
                e->t3 -= dt;
                if (e->t3 <= 0 && gap > 0 && gap < R(450) && e->t <= 0) {   /* booster: opens the gap again for a couple of seconds */
                    e->t = R(2.3f); e->t3 = R(8) + frand(m) * 4; e->speed = R(700); set_msg(m, "THE HORNET HITS HIS BOOSTER", R(1.5f)); sfx_play(0x13, 0);
                    for (int k = 0; k < 3; k++) spawn_smoke(m, e->x - r_cos(e->heading) * 30 * k, e->y - r_sin(e->heading) * 30 * k, R(10));
                }
                if (e->t > 0) target = R(700);
                e->speed += r_mul(target - e->speed, clampf(r_mul(dt, e->t > 0 ? R(4.0f) : R(1.6f)), 0, R(1)));
                e->anim += r_mul(dt, R(1.4f));
                real road_x = r_int(WORLD / 2) + r_sin(e->anim) * 85;
                if (gap > 0 && gap < R(260)) road_x -= clampf(dwrap(m->px, e->x), R(-60), R(60));   /* jinks out of your line when you close in */
                e->heading = -PI / 2 + clampf(r_mul(dwrap(road_x, e->x), R(0.006f)), R(-0.5f), R(0.5f));
                e->t2 -= dt;
                if (e->t2 <= 0 && gap > R(60) && gap < R(700)) {   /* mines out the back */
                    e->t2 = R(1.0f) + r_mul(frand(m), R(0.8f)); Ent *mn = ent_new(m); if (mn) { mn->kind = K_MINE; mn->spr = S_MINE; mn->x = e->x + (frand(m) - R(0.5f)) * 40; mn->y = e->y; mn->t = R(16); }
                }
                e->gun_t -= dt;
                if (e->gun_t <= 0 && gap > R(90) && gap < R(1500)) {   /* the rear gunner: sloppy, so the shots can be dodged */
                    e->gun_t = R(1.2f) + r_mul(frand(m), R(0.6f)); fire_shot(m, e->x, e->y, aim_at_player(m, e, R(0.4f)), R(600), false, R(16)); sfx_play(7, 0);
                }
            }
            e->tilt += r_mul(angdiff(e->heading, prev_h) * 40 - e->tilt, clampf(dt * 6, 0, R(1)));
            e->x = wrapf(e->x + r_mul_dt(r_mul(r_cos(e->heading), e->speed), dt)); e->y = wrapf(e->y + r_mul_dt(r_mul(r_sin(e->heading), e->speed), dt));
            if (d < R(70) && e->state != B_DYING) {   /* contact: a fast ram from behind dents him (the Chase H.Q. way), a side-swipe just costs you speed */
                bump_player(m, e, d, R(70));
                if (m->ram_cd <= 0) {
                    m->ram_cd = R(0.5f);
                    if (m->speed > R(330) && gap > 0) { damage_ent(m, e, R(4)); m->speed = r_mul(m->speed, R(0.55f)); m->shake = R(0.35f); set_msg(m, "RAM!", R(0.6f)); }
                    else { m->speed = r_mul(m->speed, R(0.6f)); m->shake = R(0.15f); }
                }
            }
            if (e->state == B_RUN && e->hp < r_mul(e->hp_max, R(0.4f)) && (r_trunc(m->phase_t * 6) & 1)) spawn_smoke(m, e->x, e->y, R(40));
            break; }
        default: break;
        }
    }
}

/* the placing: one more than the cars further past the start line than the player */
static void standings(Mode7 *m)
{
    int ahead = 0;
    for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_RACER && PROG(m->ents[i].lap, m->ents[i].s) > m->progress) ahead++;
    m->rank = ahead + 1;
}

/* ---------------------------------------------------------------- phases */
/* the chase: the Hornet leader runs north up the desert road with the player on his tail; catch him to start the fight */
static void begin_pursuit(Mode7 *m)
{
    ents_clear(m, false);
    build_desert(m);
    if (m->floor) r_floor_cells_changed(m->floor);
    m->hp = m->max_hp; m->hurt_t = 0; m->spin_t = 0; m->spin_dur = 0;
    m->px = r_int(WORLD / 2); m->py = r_int(WORLD / 2); m->heading = m->cam_heading = -PI / 2; m->speed = R(200);
    m->pursuit_spawn_t = R(2.5f); m->pursuit_t = 0;
    for (int i = 0; i < 160; i++) {
        Ent *e = ent_new(m); if (!e) break;
        real x = frand(m) * WORLD, y = frand(m) * WORLD;
        if (r_abs(dwrap(x, r_int(WORLD / 2))) < R(190)) { e->kind = K_NONE; continue; }
        real r = frand(m);
        e->kind = K_PROP; e->spr = r < R(0.5f) ? S_CACTUS : r < R(0.85f) ? S_ROCK_S : r < R(0.96f) ? S_ROCK_B : S_MESA; e->x = x; e->y = y;
        e->scale = e->spr == S_MESA ? R(2.2f) : e->spr == S_ROCK_B ? R(1.5f) : R(1.2f); e->solid = true;
    }
    Ent *b = ent_new(m);
    b->kind = K_BOSS; b->spr = S_LEADER; b->scale = R(1.25f); b->hp = b->hp_max = m->boss_hp_max = r_int(m->difficulty == 0 ? 60 : m->difficulty == 1 ? 80 : 100);
    b->x = r_int(WORLD / 2); b->y = m->py - R(1500); b->state = B_FLEE; b->heading = -PI / 2; b->speed = R(300); b->t = R(-10); b->t2 = R(3); b->solid = true;
    m->boss_i = (int)(b - m->ents); m->gap = R(1500);
    set_msg(m, "CATCH THE HORNET LEADER", R(3.0f));
}

/* caught him: the fight is on, but he keeps racing up the road (B_RUN) */
static void begin_boss(Mode7 *m)
{
    Ent *b = &m->ents[m->boss_i];
    b->state = B_RUN; b->t = 0; b->t2 = R(1.5f); b->t3 = R(5); b->knock = 0; b->anim = 0;
    for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_ESCORT || m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE;
    m->pursuit_spawn_t = R(7.0f);
}

static void begin_victory(Mode7 *m)
{
    m->phase = PH_VICTORY; m->phase_t = 0;   /* the car coasts to a stop by the burning wreck */
    if (plat_getenv("SABER_TRACE")) fprintf(stderr, "victory after %s s\n", RS(m->phase_t, 0));
    for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE;
    music_play(6, false); m->music_now = 6;
    dialog_open_script(&m->dlg, SCRIPT_VICTORY);
}

void mode7_update(Mode7 *m, const Input *in, real dt)
{
    if (!m->ok) { m->result = 1; return; }
    if (m->result) return;
    if (btn_pressed(in, BTN_PAUSE) && (m->phase == PH_RACE || m->phase == PH_PURSUIT || m->phase == PH_BOSS)) {
        m->paused = !m->paused; sfx_play(10, 0); music_pause(m->paused); if (m->paused) { sfx_loop(NULL); m->turbo_on = false; }
    }
    if (m->paused) return;
    { static int kill = -2; if (kill == -2) kill = plat_getenv("SABER_KILL") ? atoi(plat_getenv("SABER_KILL")) : -1; if (kill >= 0 && kill-- == 0) { m->hurt_t = 0; player_hurt(m, 99); } }   /* debug: die at step N */
    m->phase_t += dt;
    { static int last = -1, step; step++; if (plat_getenv("SABER_TRACE") && m->phase != last) { fprintf(stderr, "m7 phase %d at step %d\n", m->phase, step); last = m->phase; } }
    if (m->msg_t > 0) m->msg_t -= dt;
    if (m->hurt_t > 0) m->hurt_t -= dt;
    if (m->shake > 0) m->shake -= dt;
    if (m->ram_cd > 0) m->ram_cd -= dt;
    Input idle = { 0 }; for (int b = 0; b < BTN_COUNT; b++) idle.state[b] = 1;
    if (m->dlg.active) m->fire_hold = true; else if (!btn_down(in, BTN_SHOOT)) m->fire_hold = false;

    switch (m->phase) {
    case PH_INTRO:
        play_music(m, 10, true);   /* title_start has finished before the mode updates */
        if (m->intro_pending) { m->intro_pending = false; dialog_open_script(&m->dlg, SCRIPT_INTRO); }
        if (m->dlg.active) dialog_update(&m->dlg, in, dt);
        else { m->phase = PH_INSTRUCTIONS; m->phase_t = 0; }
        break;
    case PH_INSTRUCTIONS: {
        bool any = false; for (int b = 0; b < BTN_COUNT; b++) if (btn_pressed(in, b)) any = true;
        if (any && m->phase_t >= INSTR_OPEN_DUR) m->phase_t = INSTR_MIN_DUR;   /* skip: jump straight to the close */
        if (m->phase_t >= INSTR_MIN_DUR) { m->phase = PH_COUNTDOWN; m->phase_t = 0; m->countdown = R(3.99f); }
        break; }
    case PH_COUNTDOWN: {
        play_music(m, 10, true);   /* also covers a direct debug start */
        int before = r_trunc(m->countdown); m->countdown -= dt; int after = r_trunc(m->countdown);
        if (after != before) sfx_play(0, 0);
        update_racers(m, dt); standings(m);
        if (m->countdown <= R(1.0f)) { m->phase = PH_RACE; m->phase_t = 0; set_msg(m, "GO!", R(1.0f)); sfx_play(8, 0); }
        break; }
    case PH_RACE:
        m->race_time += dt;
        player_drive(m, in, dt, false);
        if (plat_getenv("SABER_TRACE") && r_trunc(m->race_time) != r_trunc(m->race_time - dt)) fprintf(stderr, "race t=%s lap=%d s=%s lat=%s v=%s rank=%d hp=%d turbo=%d boost=%s\n", RS(m->race_time, 0), m->lap, RS(m->s, 0), RS(m->lat, 0), RS(m->speed, 0), m->rank, m->hp, m->turbo_on, RS(m->boost, 2));
        update_racers(m, dt); update_ents(m, dt);
        standings(m);
        if (m->lap >= 3) {   /* the chequered flag after three laps: the car coasts on under the FINISH banner while the
                              * Hornets bolt off the course */
            m->phase = PH_FINISH; m->phase_t = 0; m->finish_rank = m->rank; m->msg_t = 0; sfx_loop(NULL); m->turbo_on = false;
            music_stop(); m->music_now = -1;
            for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_RACER && m->ents[i].hornet) { m->ents[i].lat_target = R(420); m->ents[i].t = R(99); }   /* off the course, no more line changes */
            for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE;
        }
        break;
    case PH_FINISH: {
        /* hands off the wheel: the car follows the track and eases off, the field rolls on past the flag */
        Input coast = { 0 }; for (int b = 0; b < BTN_COUNT; b++) coast.state[b] = 1;
        real ax, ay; track_point(m, m->s + R(240), 0, &ax, &ay, NULL);
        real want = r_atan2(dwrap(ay, m->py), dwrap(ax, m->px)), d = angdiff(want, m->heading);
        coast.state[BTN_LEFT] = d < R(-0.05f) ? 0 : 1; coast.state[BTN_RIGHT] = d > R(0.05f) ? 0 : 1;
        coast.state[BTN_UP] = m->phase_t < R(1.2f) ? 0 : 1;
        player_drive(m, &coast, dt, false);
        update_racers(m, dt); update_ents(m, dt);
        if (m->phase_t >= FINISH_DUR) {
            if (m->finish_rank <= 3) { m->phase = PH_BREAKAWAY; m->phase_t = 0; dialog_open_script(&m->dlg, SCRIPT_BREAKAWAY); }   /* the pursuit music waits for the pursuit (PH_PURSUIT), after the radio call and the target card */
            else if (m->lives <= 0) { m->phase = PH_GAMEOVER; m->phase_t = 0; music_set_volume(R(0.5f)); }   /* must rank 3rd or better: no spares left */
            else {   /* must rank 3rd or better: lose a life and start the race over from the beginning */
                m->lives--;
                start_race(m);
                m->hp = m->max_hp; m->hurt_t = 0; m->spin_t = 0; m->spin_dur = 0; m->boost = R(1); m->boost_locked = false; m->speed = 0; m->turbo_on = false; m->shake = 0; m->tilt = 0; m->fire_cd = 0; m->turbo_t = 0;
                m->phase = PH_COUNTDOWN; m->phase_t = 0; m->countdown = R(3.99f);
                play_music(m, 10, true);
                set_msg(m, "QUALIFY 3RD OR BETTER", R(3.0f));
            }
        }
        break; }
    case PH_BREAKAWAY: {
        Input coast = { 0 }; for (int b = 0; b < BTN_COUNT; b++) coast.state[b] = 1;   /* the car rolls on under the radio call */
        real ax, ay; track_point(m, m->s + R(240), 0, &ax, &ay, NULL);
        real d = angdiff(r_atan2(dwrap(ay, m->py), dwrap(ax, m->px)), m->heading);
        coast.state[BTN_LEFT] = d < R(-0.05f) ? 0 : 1; coast.state[BTN_RIGHT] = d > R(0.05f) ? 0 : 1;
        player_drive(m, &coast, dt, false);
        update_racers(m, dt); update_ents(m, dt);
        if (m->dlg.active) dialog_update(&m->dlg, in, dt);
        else { m->phase = PH_BRIEF; m->phase_t = 0; music_stop(); m->music_now = -1; sfx_play(0x13, 0); }
        break; }
    case PH_BRIEF: {   /* the Chase H.Q. target card: the lines type in, a button skips to the zoom, the zoom hands over */
        bool any = false; for (int b = 0; b < BTN_COUNT; b++) if (btn_pressed(in, b)) any = true;
        int line_prev = r_trunc(r_mul(m->phase_t - dt, R(2.2f))), line_now = r_trunc(r_mul(m->phase_t, R(2.2f)));
        if (line_now != line_prev && m->phase_t < BRIEF_TEXT_DUR - R(0.6f)) sfx_play(0, 0);   /* a blip per line */
        if (any && m->phase_t > R(0.3f) + r_div(R(5), R(2.2f)) + R(0.3f) && m->phase_t < BRIEF_TEXT_DUR) m->phase_t = BRIEF_TEXT_DUR;   /* once the lines are in */
        if (m->phase_t >= BRIEF_TEXT_DUR + BRIEF_ZOOM_DUR) { m->phase = PH_PURSUIT; m->phase_t = 0; m->white = R(1); begin_pursuit(m); }
        break; }
    case PH_PURSUIT: {
        play_music(m, 14, true);
        if (m->white > 0) m->white = clampf(m->white - r_div(dt, PURSUIT_FADE), 0, R(1));
        player_drive(m, in, dt, true);
        update_ents(m, dt);
        m->pursuit_t += dt;
        Ent *b = &m->ents[m->boss_i];
        m->gap = r_hypot(dwrap(b->x, m->px), dwrap(b->y, m->py));
        if (plat_getenv("SABER_TRACE") && r_trunc(m->pursuit_t) != r_trunc(m->pursuit_t - dt)) fprintf(stderr, "pursuit t=%s gap=%s leader %s,%s v=%s | player %s,%s v=%s\n", RS(m->pursuit_t, 0), RS(m->gap, 0), RS(b->x, 0), RS(b->y, 0), RS(b->speed, 0), RS(m->px, 0), RS(m->py, 0), RS(m->speed, 0));
        /* the escort drops back in pairs to get between the player and the leader */
        m->pursuit_spawn_t -= dt;
        if (m->pursuit_spawn_t <= 0 && m->gap > R(700)) {
            m->pursuit_spawn_t = R(3.0f) + r_mul(frand(m), R(2.0f));
            spawn_escorts(m, 1 + (frand(m) < R(0.4f) ? 1 : 0) + (m->difficulty == 2 ? 1 : 0));
        }
        if (m->gap < CATCH_GAP && dwrap(b->y, m->py) < 0) {   /* on his tail: he stops running */
            m->phase = PH_CAUGHT; m->phase_t = 0; m->speed = r_mul(m->speed, R(0.5f));
            dialog_open_script(&m->dlg, SCRIPT_CAUGHT);
        }
        break; }
    case PH_CAUGHT:
        if (m->dlg.active) dialog_update(&m->dlg, in, dt);
        else { m->phase = PH_BOSS; m->phase_t = 0; begin_boss(m); set_msg(m, "DESTROY THE HORNET LEADER", R(3.0f)); }
        break;
    case PH_BOSS: {
        play_music(m, 17, true);
        player_drive(m, in, dt, true);
        update_ents(m, dt);
        Ent *b = &m->ents[m->boss_i];
        if (b->kind != K_BOSS) { begin_victory(m); break; }
        m->gap = dwrap(b->y, m->py) < 0 ? r_hypot(dwrap(b->x, m->px), dwrap(b->y, m->py)) : 0;
        m->pursuit_spawn_t -= dt;   /* the odd escort still comes back to block */
        if (m->pursuit_spawn_t <= 0 && b->state == B_RUN) { m->pursuit_spawn_t = R(7.0f) + r_mul(frand(m), R(4.0f)); spawn_escorts(m, 1); }
        if (plat_getenv("SABER_TRACE") && (r_trunc(m->phase_t * 60) % 60) == 0) fprintf(stderr, "boss st=%d pos=%s,%s hp=%s | player %s,%s h=%s hp=%d\n", b->state, RS(b->x, 0), RS(b->y, 0), RS(b->hp, 0), RS(m->px, 0), RS(m->py, 0), RS(m->heading, 2), m->hp);
        break; }
    case PH_VICTORY:
        if (m->speed > 0) idle.state[BTN_DOWN] = 0;   /* brakes on until it stands */
        player_drive(m, &idle, dt, true); update_ents(m, dt);
        m->wreck_t -= dt;
        if (m->wreck_t <= 0) { m->wreck_t = R(0.3f); spawn_smoke(m, m->wreck_x + (frand(m) - R(0.5f)) * 40, m->wreck_y + (frand(m) - R(0.5f)) * 40, R(10)); if (frand(m) < R(0.3f)) spawn_expl(m, m->wreck_x + (frand(m) - R(0.5f)) * 40, m->wreck_y + (frand(m) - R(0.5f)) * 40, R(0.5f)); }
        if (m->phase_t > R(2.5f)) { if (m->dlg.active) dialog_update(&m->dlg, in, dt); else if (m->phase_t > R(3.0f)) { m->phase = PH_CLEARED; m->phase_t = 0; } }
        break;
    case PH_CLEARED:
        if (m->phase_t > R(1.2f)) m->result = 1;
        break;
    case PH_DEAD:
        update_ents(m, dt);
        if (m->phase_t > R(2.5f)) {
            if (m->lives <= 0) { m->phase = PH_GAMEOVER; m->phase_t = 0; music_set_volume(R(0.5f)); }
            else {
                m->lives--; m->hp = m->max_hp; m->hurt_t = R(2.0f); m->speed = 0; m->spin_t = 0; m->spin_dur = 0; m->boost = R(1); m->boost_locked = false;
                /* back onto the course / the road */
                if (m->resume_phase == PH_BOSS) { Ent *b = &m->ents[m->boss_i]; m->px = r_int(WORLD / 2); m->py = wrapf(b->y + R(500)); m->heading = m->cam_heading = -PI / 2; m->phase = PH_BOSS; b->t = 0; b->speed = R(200); for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_ESCORT || m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE; }
                else if (m->resume_phase == PH_PURSUIT) { Ent *b = &m->ents[m->boss_i]; m->px = r_int(WORLD / 2); m->py = wrapf(b->y + R(1200)); m->heading = m->cam_heading = -PI / 2; m->phase = PH_PURSUIT; for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_ESCORT || m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE; }
                else { track_point(m, m->s, 0, &m->px, &m->py, &m->heading); m->cam_heading = m->heading; m->lat = 0; m->phase = PH_RACE; for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE; }
            }
            m->phase_t = 0;
        }
        break;
    case PH_GAMEOVER:
        if (m->phase_t > R(1.5f)) m->result = 2;
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- drawing */
static void render_floor(Mode7 *m)
{
    if (!m->floor) {
        static const uint32_t *mats[T_COUNT * MIPS];
        for (int t = 0; t < T_COUNT; t++) for (int L = 0; L < MIPS; L++) mats[t * MIPS + L] = m->tiles[t][L];
        RFloorDesc d = { MAPN, MAPSH, m->cells, TEX, MIPS, T_COUNT, mats };
        m->floor = r_floor_create(m->ren, &d);
        if (!m->floor) return;
    }
    real ch = m->cam_heading, fx = r_cos(ch), fy = r_sin(ch);
    RFloorView v = { m->px - r_mul(fx, CAM_BACK), m->py - r_mul(fy, CAM_BACK), fx, fy, CAM_H, FOCAL, r_int(HORIZON),
                     HORIZON + 1, HORIZON + 1 + m->floor_h, R(0.0f), FOG0, FOG1, 256,
                     0xFF8CB2D8u,   /* ABGR (R d8, G b2, B 8c): dust at the horizon */
                     R(1.5f), m->sw };
    r_floor_draw(m->ren, m->floor, &v);
}

static void render_horizon(Mode7 *m)
{
    r_set_draw_color(m->ren, 78, 160, 214, 255);
    RFRect sky = { 0, 0, r_int(m->sw), r_int(HORIZON + 1) }; r_fill_rect(m->ren, &sky);
    if (!m->sky_ok) return;
    /* the panorama is scaled to fill the sky band exactly (no squash: both axes share one factor), then tiled at
     * its own width - it was authored to loop there, so this is a clean wrap with none of the dead columns /
     * narrow-slice repeats that reusing level 1's background layers had */
    real sc = r_int(HORIZON + 1) / m->sky_h, tw = m->sky_w * sc;
    real turn = r_div(m->cam_heading, TWO_PI);
    real ox = r_fmod(r_mul(turn, tw), tw); if (ox < 0) ox += tw;
    for (real x = -ox; x < r_int(m->sw); x += tw) {
        RFRect dst = { x, 0, tw, r_int(HORIZON + 1) };
        r_tex(m->ren, m->sky_tex, NULL, &dst);
    }
}

/* the buggy's nozzles drift a few px sideways in the hard steering poses of the clip */
static real steer_frame_shift(int frame) { return r_int(frame == 0 ? -3 : frame == 1 ? -1 : frame == 3 ? 1 : frame == 4 ? 3 : 0); }
/* which of a car's three steering poses (left / straight / right) to show */
static int steer_frame(real tilt) { return tilt < R(-0.5f) ? 0 : tilt > R(0.5f) ? 2 : 1; }

typedef struct { real d, sx, sy, scale; Ent *e; } DrawItem;
static int cmp_far(const void *a, const void *b) { real x = ((const DrawItem *)a)->d, y = ((const DrawItem *)b)->d; return x < y ? 1 : x > y ? -1 : 0; }

static void render_sprites(Mode7 *m)
{
    real ch = m->cam_heading;
    real fx = r_cos(ch), fy = r_sin(ch), rx = -fy, ry = fx;
    real camx = m->px - r_mul(fx, CAM_BACK), camy = m->py - r_mul(fy, CAM_BACK);
    DrawItem items[MAX_ENT]; int n = 0;
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &m->ents[i]; if (e->kind == K_NONE) continue;
        real dx = dwrap(e->x, camx), dy = dwrap(e->y, camy);
        real d = r_mul(dx, fx) + r_mul(dy, fy), lat = r_mul(dx, rx) + r_mul(dy, ry);
        if (d < R(12) || d > R(4200)) continue;
        real sx = r_int(m->sw) / 2 + r_muldiv(lat, FOCAL, d);   /* sw * 0.5f */
        real sy = r_int(HORIZON) + r_muldiv(CAM_H, FOCAL, d) - r_muldiv(e->z, FOCAL, d);
        real scale = r_mul(r_div(CAM_BACK, d), e->scale);
        if (sx < R(-200) || sx > r_int(m->sw + 200)) continue;
        items[n++] = (DrawItem){ d, sx, sy, scale, e };
    }
    qsort(items, n, sizeof *items, cmp_far);
    for (int i = 0; i < n; i++) {
        Ent *e = items[i].e; real fog = clampf(r_div(items[i].d - FOG0, FOG1 - FOG0), 0, R(1));
        uint8_t a = (uint8_t)r_trunc(255 * (R(1) - r_mul(fog, R(0.85f))));
        uint8_t r = e->r, g = e->g, b = e->b;
        if (e->knock > 0 && (r_trunc(e->knock * 30) & 1)) { r = 255; g = 120; b = 120; }
        int frame = e->frame;
        if (e->kind == K_RACER || e->kind == K_ESCORT || e->kind == K_BOSS) frame = steer_frame(e->tilt + angdiff(e->heading, m->cam_heading) * 2);
        draw_spr(m, e->spr, frame, items[i].sx, items[i].sy, items[i].scale, 0, r, g, b, a);
        if ((e->kind == K_RACER || e->kind == K_ESCORT) && items[i].d < R(2600) && e->hp_max > 0) {   /* a small health bar over every rival */
            real w = 20 * clampf(r_mul(items[i].scale, R(1.6f)), R(0.5f), R(1.5f)), top = items[i].sy - m->spr[e->spr].h * items[i].scale - R(5);
            real f = r_div(e->hp, e->hp_max);
            r_set_draw_blend(m->ren, R_BLEND_BLEND);
            r_set_draw_color(m->ren, 0, 0, 0, 150); RFRect bg = { r_floorr(items[i].sx - w / 2 - R(1)), r_floorr(top - R(1)), w + R(2), R(4) }; r_fill_rect(m->ren, &bg);
            r_set_draw_color(m->ren, f > R(0.5f) ? 90 : 240, f > R(0.25f) ? 220 : 80, 60, 255); RFRect fg = { r_floorr(items[i].sx - w / 2), r_floorr(top), r_mul(w, f), R(2) }; r_fill_rect(m->ren, &fg);
        }
        if (e->kind == K_BOSS && e->state != B_DYING) {   /* the target: red corner brackets around the leader, pulsing */
            real hw = m->spr[e->spr].w * items[i].scale / 2 + R(4) + r_int(2 * (r_trunc(m->phase_t * 6) & 1)), hh = m->spr[e->spr].h * items[i].scale + R(8);
            real x0 = r_floorr(items[i].sx - hw), x1 = r_floorr(items[i].sx + hw), y1 = r_floorr(items[i].sy + R(4)), y0 = r_floorr(y1 - hh);
            real L = clampf(r_mul(hw, R(0.4f)), R(3), R(10));
            r_set_draw_blend(m->ren, R_BLEND_NONE); r_set_draw_color(m->ren, 255, 50, 50, 255);
            const real two = R(2);
            RFRect q[8] = { { x0, y0, L, two }, { x0, y0, two, L }, { x1 - L, y0, L, two }, { x1 - two, y0, two, L },
                               { x0, y1 - two, L, two }, { x0, y1 - L, two, L }, { x1 - L, y1 - two, L, two }, { x1 - two, y1 - L, two, L } };
            r_fill_rects(m->ren, q, 8);
        }
    }
}

static void render_player(Mode7 *m)
{
    if (m->phase == PH_DEAD || m->phase == PH_GAMEOVER) return;   /* the wreck stays gone */
    real sy = r_int(HORIZON) + r_div(r_mul(CAM_H, FOCAL), CAM_BACK) - R(6) - m->bounce;
    real sx = r_int(m->sw) / 2 + r_mul(m->tilt, R(1.2f));
    if (m->shake > 0) { sx += r_int(rand() % 5 - 2); sy += r_int(rand() % 3 - 1); }
    /* the clip is a steering set: hard left, left, straight, right, hard right */
    int frame = m->tilt < R(-5) ? 0 : m->tilt < R(-1.5f) ? 1 : m->tilt <= R(1.5f) ? 2 : m->tilt <= R(5) ? 3 : 4;
    uint8_t r = 255, g = 255, b = 255;
    if (m->hurt_t > 0 && (r_trunc(m->hurt_t * 20) & 1)) { r = 255; g = 90; b = 90; }
    real ang = 0;
    if (m->spin_dur > 0) { real p = R(1) - r_div(m->spin_t, m->spin_dur); ang = r_mul(360 * p, R(2) - p); }   /* one whole turn, easing out: 0 and 360 are the same pose */
    draw_spr(m, S_BUGGY, frame, sx, sy, R(1), ang, r, g, b, 255);
    if (m->turbo_on && ang == 0) {   /* the afterburner: a flame over each exhaust nozzle (10x10 at (23,21) and (60,21) of the sprite) */
        Spr *bs = &m->spr[S_BUGGY]; real x0 = r_floorr(sx - r_int(bs->w) / 2), y0 = r_floorr(sy - r_int(bs->h));
        int fr = r_trunc(m->turbo_t * 18) & 3;
        static const real NOZ[2][2] = { { R(23), R(21) }, { R(60), R(21) } };
        for (int k = 0; k < 2; k++) {
            real fx = x0 + NOZ[k][0] + R(5) + (steer_frame_shift(frame)) - R(6.0f), fy = y0 + NOZ[k][1] + R(10);   /* -6px: the flames sat right of the nozzles */
            draw_spr(m, S_TURBO, fr, fx, fy, R(1), 0, 255, 255, 255, 255);
            draw_spr(m, S_TURBO, (fr + 2) & 3, fx, fy + R(1), R(1.4f), 0, 255, 255, 255, 110);   /* a soft halo behind it */
        }
    }
}

static void bar(Ren *r, real x, real y, real w, real h, real f, uint8_t cr, uint8_t cg, uint8_t cb)
{
    r_set_draw_blend(r, R_BLEND_BLEND);
    r_set_draw_color(r, 0, 0, 0, 160); RFRect bg = { x - R(1), y - R(1), w + R(2), h + R(2) }; r_fill_rect(r, &bg);
    r_set_draw_color(r, cr, cg, cb, 255); RFRect fg = { x, y, r_mul(w, clampf(f, 0, R(1))), h }; r_fill_rect(r, &fg);
}

/* the chequered flag: a band of black / white squares rolls across the middle of the screen with FINISH on it, the
 * placing under it */
static void render_finish(Mode7 *m, Font *f, Font *small)
{
    real t = m->phase_t; int sw = m->sw;
    real in = clampf(r_div(t, R(0.35f)), 0, R(1)); in = R(1) - r_mul(R(1) - in, R(1) - in);
    real out = clampf(r_div(t - (FINISH_DUR - R(0.4f)), R(0.4f)), 0, R(1));
    real bx = -sw * (R(1) - in) + r_mul(sw * out, out);   /* rolls in from the left, leaves to the right */
    const int CS = 10; real y0 = R(64);
    r_set_draw_blend(m->ren, R_BLEND_NONE);
    for (int row = 0; row < 2; row++) for (int col = -1; col <= sw / CS + 1; col++) {
        int scroll = r_trunc(t * 60) / CS;
        bool white = ((col + row + scroll) & 1) == 0;
        r_set_draw_color(m->ren, white ? 245 : 20, white ? 245 : 20, white ? 245 : 24, 255);
        RFRect q = { bx + r_int(col * CS), y0 + r_int(row * CS), r_int(CS), r_int(CS) }; r_fill_rect(m->ren, &q);
        RFRect q2 = { bx + r_int(col * CS), y0 + R(52) + r_int(row * CS), r_int(CS), r_int(CS) }; r_fill_rect(m->ren, &q2);
    }
    r_set_draw_blend(m->ren, R_BLEND_BLEND); r_set_draw_color(m->ren, 0, 0, 0, 170);
    RFRect mid = { bx, y0 + R(20), r_int(sw), R(32) }; r_fill_rect(m->ren, &mid);
    const char *fin = "FINISH!"; real fw = r_int(font_text_width(f, fin));
    font_draw(f, fin, bx + r_int(sw) / 2 - fw / 2 + R(1), y0 + R(25), 40, 30, 0);   /* / 2: * 0.5f */
    font_draw(f, fin, bx + r_int(sw) / 2 - fw / 2, y0 + R(24), 255, 210, 40);
    static const char *const ORD[] = { "1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH" };
    char buf[32]; int rk = m->finish_rank < 1 ? 1 : m->finish_rank > 8 ? 8 : m->finish_rank;
    snprintf(buf, sizeof buf, "%s PLACE", ORD[rk - 1]);
    if (t > R(0.8f)) font_draw(small, buf, bx + r_int(sw) / 2 - r_int(font_text_width(small, buf)) / 2, y0 + R(40), 255, 255, 255);
    if (t > R(1.6f) && (r_trunc(t * 3) & 1)) {
        const char *w = rk <= 3 ? "THE HORNETS ARE LEAVING THE COURSE!" : "MUST FINISH 3RD OR BETTER!";
        font_draw(small, w, r_int(sw) / 2 - r_int(font_text_width(small, w)) / 2, R(140), 255, 90, 90);
    }
}

/* the Chase H.Q. target briefing: a black card, a blue console panel whose lines type in one by one with the
 * Hornet leader's car in a pulsing red reticle at the right; at the end the target zooms into the camera under a
 * white-out and the pursuit fades in from that white (m->white, PH_PURSUIT) */
static void render_brief(Mode7 *m, Font *f, Font *small)
{
    real t = m->phase_t; int sw = m->sw, sh = m->sh;
    r_set_draw_blend(m->ren, R_BLEND_NONE);
    r_set_draw_color(m->ren, 0, 0, 0, 255); RFRect all = { 0, 0, r_int(sw), r_int(sh) }; r_fill_rect(m->ren, &all);
    /* the panel wipes open from the middle in the first 0.3 s */
    real open = clampf(r_div(t, R(0.3f)), 0, R(1)); open = R(1) - r_mul(R(1) - open, R(1) - open);
    bool narrow = sw < 400;   /* 4:3: no room beside the lines, so the panel grows and the target sits under them */
    real ph = (narrow ? 200 : 150) * open, py = (r_int(sh) - ph) / 2;   /* / 2: * 0.5f */
    r_set_draw_color(m->ren, 10, 18, 44, 255); RFRect panel = { R(16), py, r_int(sw - 32), ph }; r_fill_rect(m->ren, &panel);
    r_set_draw_color(m->ren, 60, 120, 220, 255); RFRect top = { R(16), py - R(2), r_int(sw - 32), R(2) }, bot = { R(16), py + ph, r_int(sw - 32), R(2) }; r_fill_rect(m->ren, &top); r_fill_rect(m->ren, &bot);
    r_set_draw_blend(m->ren, R_BLEND_BLEND); r_set_draw_color(m->ren, 60, 120, 220, 40);
    for (real y = py; y < py + ph; y += R(3)) { RFRect ln = { R(16), y, r_int(sw - 32), R(1) }; r_fill_rect(m->ren, &ln); }   /* console scanlines */
    if (open < R(1)) return;
    /* the lines, typed in at 2.2 lines / s */
    static const char *const LINES[] = { "CAVALRY COMMAND - ALERT", "TARGET:  BLACK HORNET LEADER", "VEHICLE: HORNET RACING BUGGY", "HEADING: NORTH - DOME CITY", "ORDERS:  PURSUE AND DESTROY" };
    int nl = (int)(sizeof LINES / sizeof *LINES); real tl = t - R(0.3f);
    for (int i = 0; i < nl; i++) {
        real lt = tl - r_div(r_int(i), R(2.2f)); if (lt < 0) break;
        int len = (int)strlen(LINES[i]), shown = r_trunc(lt * 40); if (shown > len) shown = len;
        uint8_t r = i == 0 ? 255 : i == nl - 1 ? 255 : 200, g = i == 0 ? 182 : i == nl - 1 ? 90 : 220, b = i == 0 ? 0 : i == nl - 1 ? 90 : 255;
        font_draw_n(small, LINES[i], shown, R(30), py + R(12) + r_int(i * 16), r, g, b);
        if (shown < len && (r_trunc(t * 12) & 1)) { r_set_draw_color(m->ren, 200, 220, 255, 255); RFRect cur = { r_int(30 + font_text_width_n(small, LINES[i], shown)), py + R(12) + r_int(i * 16), R(6), r_int(small->h) }; r_fill_rect(m->ren, &cur); }
    }
    if (tl > r_div(r_int(nl), R(2.2f)) + R(0.3f)) {
        const char *go = t < BRIEF_TEXT_DUR ? "PRESS A BUTTON" : "GO!";
        if ((r_trunc(t * 4) & 1) || t >= BRIEF_TEXT_DUR) font_draw(f, go, R(30), py + ph - R(26), 255, 255, 255);
    }
    /* the target: the leader's car in the reticle, then the zoom */
    real zt = clampf(r_div(t - BRIEF_TEXT_DUR, BRIEF_ZOOM_DUR), 0, R(1)), zz = r_mul(r_mul(zt, zt), zt);
    real sc0 = narrow ? R(1.0f) : R(1.6f), cx0 = r_int(narrow ? sw - 80 : sw - 78), cy0 = narrow ? py + ph - R(12) : py + ph / 2 + R(28);
    real cx = cx0 + r_mul(r_int(sw) / 2 - cx0, zt), cy = cy0 + r_mul(r_int(sh) / 2 + R(60) - cy0, zt);
    real sc = sc0 + r_mul(R(14.0f), zz);
    /* a pulsing red reticle around the car's silhouette, the scan bar rolling down it */
    Spr *ls = &m->spr[S_LEADER]; real hw = ls->w * sc / 2 + R(6) + r_int(2 * (r_trunc(t * 6) & 1)), hh = ls->h * sc + R(10);
    r_set_draw_color(m->ren, 60, 20, 30, 255); RFRect bg = { cx - hw, cy - hh + R(2), hw * 2, hh }; r_fill_rect(m->ren, &bg);
    draw_spr(m, S_LEADER, 1, cx, cy, sc, 0, 255, 255, 255, 255);
    real x0 = cx - hw, x1 = cx + hw, y1 = cy + R(4), yy0 = y1 - hh, L = clampf(r_mul(hw, R(0.4f)), R(4), R(14));
    r_set_draw_color(m->ren, 255, 50, 50, 255);
    const real two = R(2);
    RFRect q[8] = { { x0, yy0, L, two }, { x0, yy0, two, L }, { x1 - L, yy0, L, two }, { x1 - two, yy0, two, L }, { x0, y1 - two, L, two }, { x0, y1 - L, two, L }, { x1 - L, y1 - two, L, two }, { x1 - two, y1 - L, two, L } };
    r_fill_rects(m->ren, q, 8);
    if (zt == 0) { r_set_draw_color(m->ren, 255, 80, 80, 120); RFRect scan = { x0, yy0 + r_fmod(t * 40, hh), hw * 2, R(2) }; r_fill_rect(m->ren, &scan); }
    if (tl > R(1.0f) && zt == 0) font_draw(small, "TARGET", cx - r_int(font_text_width(small, "TARGET")) / 2, yy0 - R(12), 255, 60, 60);
    if (zt > 0) { r_set_draw_color(m->ren, 255, 255, 255, (uint8_t)r_trunc(255 * zz)); r_fill_rect(m->ren, &all); }
}

static void render_hud(Mode7 *m)
{
    Font *f = font_get(0x4058897F), *small = font_get(0x12072E60);
    if (!f || !small) return;
    char buf[64];
    /* the car: damage meter + spare cars (it is Fireball's buggy whoever drives, so no hero portrait / hearts here) */
    {
        real hp = clampf(r_int(m->hp) / m->max_hp, 0, R(1));
        font_draw(small, "RED FURY", R(8), R(6), 255, 182, 0);
        bar(m->ren, R(8), R(16), R(72), R(6), hp, hp > R(0.5f) ? 90 : 240, hp > R(0.25f) ? 220 : 80, 60);
        if (m->hurt_t > 0 && (r_trunc(m->hurt_t * 20) & 1)) bar(m->ren, R(8), R(16), R(72), R(6), R(1), 255, 255, 255);
        draw_spr(m, S_BUGGY, 2, R(18), R(40), R(0.28f), 0, 255, 255, 255, 255);
        snprintf(buf, sizeof buf, "x%d", m->lives);
        font_draw(small, buf, R(30), R(30), 255, 255, 255);
    }
    /* turbo meter + speed (hidden under a dialog box, which sits on the same rows and covers them in 4:3) */
    if (!m->dlg.active) {
        if (m->boost_locked) bar(m->ren, R(8), r_int(m->sh - 14), R(60), R(5), m->boost, 230, 50, 50);   /* used up: red until halfway */
        else bar(m->ren, R(8), r_int(m->sh - 14), R(60), R(5), m->boost, 255, 182, 0);
        font_draw(small, "TURBO", R(8), r_int(m->sh - 26), 255, 182, 0);
        snprintf(buf, sizeof buf, "%3d", r_trunc(r_mul(r_abs(m->speed), R(0.6f))));
        font_draw(f, buf, r_int(m->sw - 8 - font_text_width(f, buf)), r_int(m->sh - 20), 255, 255, 255);
        font_draw(small, "KM/H", r_int(m->sw - 12 - font_text_width(small, "KM/H")), r_int(m->sh - 30), 200, 200, 200);
    }
    if (m->phase == PH_RACE || m->phase == PH_COUNTDOWN || m->phase == PH_FINISH || m->phase == PH_BREAKAWAY) {
        static const char *const ORD[] = { "1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH" };
        snprintf(buf, sizeof buf, "LAP %d/3", m->lap + 1 > 3 ? 3 : m->lap + 1);
        font_draw(f, buf, r_int(m->sw - 8 - font_text_width(f, buf)), R(8), 255, 255, 255);
        int rk = m->rank < 1 ? 1 : m->rank > 8 ? 8 : m->rank;
        font_draw(f, ORD[rk - 1], r_int(m->sw - 8 - font_text_width(f, ORD[rk - 1])), R(24), rk == 1 ? 255 : 255, rk == 1 ? 182 : 255, rk == 1 ? 0 : 255);
        snprintf(buf, sizeof buf, "%d'%02d\"%02d", r_trunc(m->race_time) / 60, r_trunc(m->race_time) % 60, centis(m->race_time) % 100);
        font_draw(small, buf, r_int(m->sw / 2 - font_text_width(small, buf) / 2), R(8), 255, 255, 255);
        if (m->phase == PH_COUNTDOWN) {
            int c = r_trunc(m->countdown); if (c >= 1 && c <= 3) { snprintf(buf, sizeof buf, "%d", c); font_draw(f, buf, r_int(m->sw / 2 - font_text_width(f, buf) / 2), R(70), 255, 60, 60); }
        }
    } else if (m->phase == PH_PURSUIT || m->phase == PH_CAUGHT || m->phase == PH_BOSS) {
        Ent *b = &m->ents[m->boss_i];
        font_draw(small, "HORNET LEADER", r_int(m->sw / 2 - font_text_width(small, "HORNET LEADER") / 2), R(6), 255, 120, 120);
        if (m->phase == PH_PURSUIT) {   /* how close you are to catching him */
            bar(m->ren, r_int(m->sw / 2 - 70), R(18), R(140), R(5), R(1.0f) - clampf(r_div(m->gap - CATCH_GAP, GAP_MAX - CATCH_GAP), 0, R(1)), 255, 182, 0);
            snprintf(buf, sizeof buf, "GAP %4dM", r_trunc(m->gap));
            font_draw(small, buf, r_int(m->sw - 8 - font_text_width(small, buf)), R(8), 255, 255, 255);
        } else bar(m->ren, r_int(m->sw / 2 - 70), R(18), R(140), R(5), b->kind == K_BOSS ? r_div(b->hp, m->boss_hp_max) : 0, 230, 50, 50);
        if (b->kind == K_BOSS) {   /* where is he? a red arrow along the screen edge when the leader is off screen */
            real rel = angdiff(r_atan2(dwrap(b->y, m->py), dwrap(b->x, m->px)), m->cam_heading);
            if (r_abs(rel) > R(0.75f)) {
                real cx = r_int(m->sw) / 2 + r_mul(r_sin(rel), r_mul(r_int(m->sw), R(0.45f))), cy = R(60) - r_cos(rel) * 40 + R(60);
                r_set_draw_color(m->ren, 255, 60, 60, 255);
                RFRect q = { cx - R(4), cy - R(4), R(8), R(8) }; r_fill_rect(m->ren, &q);
                font_draw(small, rel > 0 ? ">" : "<", cx + r_int(rel > 0 ? 6 : -12), cy - R(5), 255, 60, 60);
            }
        }
    }
    if (m->msg_t > 0) font_draw(f, m->msg, r_int(m->sw / 2 - font_text_width(f, m->msg) / 2), R(96), 255, 255, 255);
    if (m->phase == PH_FINISH) render_finish(m, f, small);
    if (m->paused) {
        r_set_draw_blend(m->ren, R_BLEND_BLEND); r_set_draw_color(m->ren, 0, 0, 0, 64);
        RFRect q = { 0, 0, r_int(m->sw), r_int(m->sh) }; r_fill_rect(m->ren, &q);
        Sprite *ps = sprite_get(0xB2143E42);
        if (ps && ((plat_ticks_ms() / 16) & 0x7f) > 0x30) sprite_draw(ps, 0, r_int((m->sw - ps->w) / 2), r_int((m->sh - ps->h) / 2), false);
    }
}

/* the controls card: the grid scene dims behind it while the panel wipes open from the middle (matches
 * render_brief's grow), sized off sw/sh so it reads the same in 16:9 and 4:3; any button skips it once open,
 * and it times out on its own after INSTR_MIN_DUR so nothing can get stuck on it */
static void render_instructions(Mode7 *m, Font *f, Font *small)
{
    real t = m->phase_t; int sw = m->sw, sh = m->sh;
    real open = clampf(r_div(t, INSTR_OPEN_DUR), 0, R(1)); open = R(1) - r_mul(R(1) - open, R(1) - open);
    r_set_draw_blend(m->ren, R_BLEND_BLEND);
    r_set_draw_color(m->ren, 0, 0, 0, (uint8_t)r_trunc(140 * open));
    RFRect scrim = { 0, 0, r_int(sw), r_int(sh) }; r_fill_rect(m->ren, &scrim);
    static const struct { const char *label, *desc; } LINES[] = {
        { "STEER", "Left / Right" }, { "ACCELERATE", "Jump button or Up" }, { "FIRE", "Shoot button" },
        { "TURBO", "Aim button" }, { "BRAKE", "Down" },
    };
    int nl = (int)(sizeof LINES / sizeof *LINES);
    real full_h = r_int(34 + nl * 14 + 22);
    real pw = r_int(sw) - R(40), ph = r_mul(full_h, open), px0 = R(20), py0 = (r_int(sh) - full_h) / 2 + (full_h - ph) / 2;   /* / 2: * 0.5f */
    r_set_draw_color(m->ren, 10, 18, 44, (uint8_t)r_trunc(255 * open));
    RFRect panel = { px0, py0, pw, ph }; r_fill_rect(m->ren, &panel);
    r_set_draw_color(m->ren, 60, 120, 220, (uint8_t)r_trunc(255 * open));
    RFRect top = { px0, py0 - R(2), pw, R(2) }, bot = { px0, py0 + ph, pw, R(2) };
    r_fill_rect(m->ren, &top); r_fill_rect(m->ren, &bot);
    if (open < R(1) || !f || !small) return;
    const char *title = "ALL GALAXY GRAND PRIX";
    font_draw(f, title, px0 + (pw - r_int(font_text_width(f, title))) / 2, py0 + R(8), 255, 182, 0);
    for (int i = 0; i < nl; i++) {
        real ly = py0 + R(30) + r_int(i * 14);
        font_draw(small, LINES[i].label, px0 + R(14), ly, 255, 224, 192);
        font_draw(small, LINES[i].desc, px0 + R(110), ly, 220, 230, 255);
    }
    const char *sub = "THREE LAPS!";
    font_draw(small, sub, px0 + (pw - r_int(font_text_width(small, sub))) / 2, py0 + full_h - R(26), 255, 255, 255);
    if (t >= INSTR_OPEN_DUR && (r_trunc(t * 4) & 1)) {
        const char *skip = "PRESS A BUTTON TO SKIP";
        font_draw(small, skip, px0 + (pw - r_int(font_text_width(small, skip))) / 2, py0 + full_h - R(12), 200, 200, 200);
    }
}

void mode7_draw(Mode7 *m, bool scanlines)
{
    if (!m->ok) return;
    if (m->phase == PH_BRIEF) { Font *f = font_get(0x4058897F), *small = font_get(0x12072E60); if (f && small) render_brief(m, f, small); return; }
    render_horizon(m);
    render_floor(m);
    render_sprites(m);
    render_player(m);
    render_hud(m);
    if (m->dlg.active) dialog_draw(&m->dlg, m->ren, m->sw, m->sh);
    if (m->phase == PH_INSTRUCTIONS) { Font *f = font_get(0x4058897F), *small = font_get(0x12072E60); render_instructions(m, f, small); }
    if (m->white > 0) { r_set_draw_blend(m->ren, R_BLEND_BLEND); r_set_draw_color(m->ren, 255, 255, 255, (uint8_t)r_trunc(255 * m->white)); RFRect q = { 0, 0, r_int(m->sw), r_int(m->sh) }; r_fill_rect(m->ren, &q); }
    if (scanlines) gfx_scanlines(m->sw, m->sh);
    if (m->phase == PH_GAMEOVER || m->phase == PH_CLEARED) {
        real a = clampf(r_div(m->phase_t, m->phase == PH_CLEARED ? R(1.2f) : R(1.5f)), 0, R(1)); uint8_t v = m->phase == PH_CLEARED ? 255 : 0;
        r_set_draw_blend(m->ren, R_BLEND_BLEND); r_set_draw_color(m->ren, v, v, v, (uint8_t)r_trunc(a * 255));
        RFRect q = { 0, 0, r_int(m->sw), r_int(m->sh) }; r_fill_rect(m->ren, &q);
    }
}
