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
#include <math.h>

#define PI 3.1415927f
#define TWO_PI 6.2831853f

/* ---- projection ---- */
#define HORIZON 112          /* screen row of the horizon (floor rows HORIZON+1 .. sh-1) */
#define CAM_H 48.0f          /* camera height in world units */
#define FOCAL 210.0f         /* pixels */
#define CAM_BACK 92.0f       /* the player's ground point is this far in front of the camera */
#define FOG0 1400.0f
#define FOG1 3400.0f

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
    int kind, spr, frame; float anim;
    float x, y, z, heading, speed, vx, vy;
    float hp, hp_max, t, t2, t3, gun_t, scale, tilt;
    int state; bool player_owned, hornet, solid;
    /* racers: on rails along the track */
    float s, lat, lat_target, max_speed; int lap, id; float knock;
    uint8_t r, g, b;   /* colour mod */
} Ent;
#define MAX_ENT 200

enum { PH_INTRO, PH_INSTRUCTIONS, PH_COUNTDOWN, PH_RACE, PH_FINISH, PH_BREAKAWAY, PH_BRIEF, PH_PURSUIT, PH_CAUGHT, PH_BOSS, PH_VICTORY, PH_DEAD, PH_GAMEOVER, PH_CLEARED };
/* the chequered flag -> the Hornets' breakaway -> the Chase H.Q. style target briefing -> zoom into the pursuit */
#define FINISH_DUR 3.2f
#define BRIEF_TEXT_DUR 5.2f   /* the briefing's lines have typed in and held */
#define BRIEF_ZOOM_DUR 0.9f   /* the target zooms into the camera under a white-out */
#define PURSUIT_FADE 1.1f     /* the desert fades in from white */
#define CATCH_GAP 260.0f     /* the pursuit ends when the leader is this close */
#define GAP_MAX 4200.0f      /* HUD gap bar full scale */
#define INSTR_OPEN_DUR 0.5f   /* the controls card grows/fades in before it can be skipped */
#define INSTR_MIN_DUR 2.2f    /* minimum time on screen even with no input, so it isn't a flash-frame */

#define TRACK_N 1024
#define N_RACERS 7

struct Mode7 {
    SDL_Renderer *ren; int sw, sh;
    SDL_Texture *atlas; Spr spr[S_COUNT]; bool ok;
    uint32_t tiles[T_COUNT][MIPS][TEX * TEX];   /* level L is (TEX >> L) square */
    uint8_t cells[MAPN * MAPN];
    SDL_Texture *floor_tex; uint32_t *floor_px; int floor_h;
    SDL_Texture *sky_tex; int sky_w, sky_h; bool sky_ok;
    /* track */
    float tx[TRACK_N], ty[TRACK_N], tlen[TRACK_N], track_len;
    /* player */
    float px, py, heading, speed, cam_heading, vx, vy;
    int hp, max_hp, lives, difficulty;
    float boost, fire_cd, hurt_t, spin_t, bounce, anim_t, shake, tilt;
    float s, lat; int lap, rank, near_idx; float progress, last_s;
    /* phase */
    int phase; float phase_t, countdown;
    Dialog dlg; bool paused; int result;
    Ent ents[MAX_ENT];
    /* pursuit */
    float gap, pursuit_spawn_t, pursuit_t;
    /* boss */
    float boss_hp_max; int boss_i; float ram_cd; float wreck_x, wreck_y, wreck_t;
    float race_time; int kills;
    unsigned rng;
    float dead_t; int resume_phase;
    char msg[64]; float msg_t;
    int music_now;
    bool intro_pending;        /* the intro dialog opens on the first update (after the level title card) */
    bool turbo_on; float turbo_t; int finish_rank;   /* turbo: lit last frame / flame animation clock; finish: the placing at the flag */
    bool boost_locked;   /* turbo used up: no boost again until the meter recharges halfway */
    float white;               /* full-screen white veil alpha (the zoom into the pursuit) */
};

/* ---------------------------------------------------------------- helpers */
static float frand(Mode7 *m) { m->rng = m->rng * 1664525u + 1013904223u; return (m->rng >> 8) / 16777216.0f; }
static float wrapf(float v) { v = fmodf(v, (float)WORLD); return v < 0 ? v + WORLD : v; }
static float angdiff(float a, float b) { float d = fmodf(a - b + PI, TWO_PI); if (d < 0) d += TWO_PI; return d - PI; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* shortest wrapped delta */
static float dwrap(float a, float b) { float d = a - b; if (d > WORLD / 2) d -= WORLD; if (d < -WORLD / 2) d += WORLD; return d; }
static void set_msg(Mode7 *m, const char *s, float t) { snprintf(m->msg, sizeof m->msg, "%s", s); m->msg_t = t; }
static void play_music(Mode7 *m, int track, bool loop) { if (m->music_now != track) { music_play(track, loop); m->music_now = track; music_set_volume(1.0f); } }

static Ent *ent_new(Mode7 *m)
{
    for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_NONE) { Ent *e = &m->ents[i]; memset(e, 0, sizeof *e); e->scale = 1; e->r = e->g = e->b = 255; return e; }
    return NULL;
}
static void ents_clear(Mode7 *m, bool keep_player_stuff) { (void)keep_player_stuff; memset(m->ents, 0, sizeof m->ents); }

static uint8_t cell_at(const Mode7 *m, float x, float y) { int cx = ((int)floorf(x) >> MAPSH) & (MAPN - 1), cy = ((int)floorf(y) >> MAPSH) & (MAPN - 1); return m->cells[cy * MAPN + cx]; }
static void cell_set(Mode7 *m, int cx, int cy, uint8_t t) { m->cells[(cy & (MAPN - 1)) * MAPN + (cx & (MAPN - 1))] = t; }
static bool is_road(uint8_t t) { return t == T_ASPHALT || t == T_LINE || t == T_CHECKER || t == T_DASH || t == T_DIRT; }
static bool is_rumble(uint8_t t) { return t == T_KERB_RED || t == T_KERB_WHITE || t == T_SHOULDER; }

static void spawn_expl(Mode7 *m, float x, float y, float scale)
{
    Ent *e = ent_new(m); if (!e) return;
    e->kind = K_EXPL; e->spr = S_EXPL; e->x = x; e->y = y; e->scale = scale; e->t = 0;
}
static void spawn_smoke(Mode7 *m, float x, float y, float z)
{
    Ent *e = ent_new(m); if (!e) return;
    e->kind = K_SMOKE; e->spr = S_SMOKE; e->x = x; e->y = y; e->z = z; e->scale = 1;
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
    FILE *f = fopen(txt, "r");
    char name[64]; int x, y, sw, sh, n; int found = 0;
    while (f && fscanf(f, "%63s %d %d %d %d %d", name, &x, &y, &sw, &sh, &n) == 6) {
        for (int i = 0; i < S_COUNT; i++) if (!strcmp(name, SPR_NAMES[i])) { m->spr[i] = (Spr){ x, y, sw, sh, n }; found++; }
    }
    if (f) fclose(f);
    if (found < S_COUNT) fprintf(stderr, "mode7.txt: %d/%d sprites\n", found, S_COUNT);
    m->atlas = SDL_CreateTexture(m->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    SDL_UpdateTexture(m->atlas, NULL, px, w * 4);
    SDL_SetTextureBlendMode(m->atlas, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(m->atlas, SDL_SCALEMODE_NEAREST);
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
    int w, h; uint32_t *px = png_load_rgba(png, &w, &h);
    if (!px) return false;
    m->sky_tex = SDL_CreateTexture(m->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    SDL_UpdateTexture(m->sky_tex, NULL, px, w * 4);
    SDL_SetTextureScaleMode(m->sky_tex, SDL_SCALEMODE_NEAREST);
    m->sky_w = w; m->sky_h = h;
    free(px);
    return true;
}

/* draw an atlas sprite, bottom-centred at (x, y_bottom), scaled, optional rotation and colour mod */
static void draw_spr(Mode7 *m, int id, int frame, float cx, float ybot, float scale, float angle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    Spr *s = &m->spr[id]; if (s->frames < 1) return;
    if (frame < 0) frame = 0;
    if (frame >= s->frames) frame = s->frames - 1;
    SDL_FRect src = { (float)(s->x + frame * s->w), (float)s->y, (float)s->w, (float)s->h };
    float w = s->w * scale, h = s->h * scale;
    SDL_FRect dst = { floorf(cx - w * 0.5f), floorf(ybot - h), w, h };
    if (dst.x + w < 0 || dst.x > m->sw || dst.y + h < 0 || dst.y > m->sh) return;
    SDL_SetTextureColorMod(m->atlas, r, g, b); SDL_SetTextureAlphaMod(m->atlas, a);
    if (angle != 0) SDL_RenderTextureRotated(m->ren, m->atlas, &src, &dst, angle, NULL, SDL_FLIP_NONE);
    else SDL_RenderTexture(m->ren, m->atlas, &src, &dst);
    SDL_SetTextureColorMod(m->atlas, 255, 255, 255); SDL_SetTextureAlphaMod(m->atlas, 255);
}

/* ---------------------------------------------------------------- worlds */
static void fill_sand(Mode7 *m)
{
    for (int i = 0; i < MAPN * MAPN; i++) m->cells[i] = frand(m) < 0.5f ? T_SAND : T_SAND2;
    /* scorched patches: a few hundred blobs a few cells across */
    for (int k = 0; k < 400; k++) {
        int cx = (int)(frand(m) * MAPN), cy = (int)(frand(m) * MAPN), r = 2 + (int)(frand(m) * 4);
        for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) if (x * x + y * y <= r * r) cell_set(m, cx + x, cy + y, T_SAND_DARK);
    }
}

/* the New Borderland circuit: a Catmull-Rom loop through hand-placed control points, sampled into TRACK_N points */
static void build_track(Mode7 *m)
{
    /* the start line sits on the long back straight (CP 0), the grid forms up behind it on the same straight */
    static const float CP[][2] = {
        { 600, 0 }, { 1500, 0 }, { 2300, 250 }, { 2700, 900 }, { 2400, 1500 }, { 1700, 1800 }, { 1000, 1500 }, { 500, 1950 },
        { -300, 2250 }, { -1200, 1950 }, { -1750, 1250 }, { -1500, 500 }, { -1950, -300 }, { -1350, -950 }, { -700, -650 }, { -300, -80 },
    };
    int n = sizeof CP / sizeof CP[0];
    float ox = WORLD * 0.5f, oy = WORLD * 0.5f;
    for (int i = 0; i < TRACK_N; i++) {
        float u = (float)i / TRACK_N * n; int k = (int)u; float t = u - k;
        const float *p0 = CP[(k - 1 + n) % n], *p1 = CP[k % n], *p2 = CP[(k + 1) % n], *p3 = CP[(k + 2) % n];
        float t2 = t * t, t3 = t2 * t;
        float x = 0.5f * ((2 * p1[0]) + (-p0[0] + p2[0]) * t + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 + (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3);
        float y = 0.5f * ((2 * p1[1]) + (-p0[1] + p2[1]) * t + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 + (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3);
        m->tx[i] = ox + x; m->ty[i] = oy + y;
    }
    m->tlen[0] = 0;
    for (int i = 1; i <= TRACK_N; i++) {
        int a = i - 1, b = i % TRACK_N;
        float d = hypotf(m->tx[b] - m->tx[a], m->ty[b] - m->ty[a]);
        if (i < TRACK_N) m->tlen[i] = m->tlen[a] + d; else m->track_len = m->tlen[a] + d;
    }
    /* rasterize: every map cell near the track remembers its nearest sample (distance + index), then the distance
     * from the centreline picks the material and the sample's arc length phases the kerb / dash pattern */
    fill_sand(m);
    const float HALF = 104, LINE_W = 4, KERB = 118;
    float *dist = malloc(sizeof(float) * MAPN * MAPN); int16_t *near = malloc(sizeof(int16_t) * MAPN * MAPN);
    for (int i = 0; i < MAPN * MAPN; i++) { dist[i] = 1e9f; near[i] = -1; }
    const int R = (int)(KERB / (1 << MAPSH)) + 1;
    for (int i = 0; i < TRACK_N; i++) {
        int ccx = (int)floorf(m->tx[i]) >> MAPSH, ccy = (int)floorf(m->ty[i]) >> MAPSH;
        for (int cy = ccy - R; cy <= ccy + R; cy++) for (int cx = ccx - R; cx <= ccx + R; cx++) {
            float wx = (cx << MAPSH) + (1 << MAPSH) * 0.5f, wy = (cy << MAPSH) + (1 << MAPSH) * 0.5f;
            float d = hypotf(wx - m->tx[i], wy - m->ty[i]);
            int idx = (cy & (MAPN - 1)) * MAPN + (cx & (MAPN - 1));
            if (d < dist[idx]) { dist[idx] = d; near[idx] = (int16_t)i; }
        }
    }
    for (int idx = 0; idx < MAPN * MAPN; idx++) {
        float d = dist[idx]; if (d >= KERB) continue;
        float along = m->tlen[near[idx]];
        uint8_t t;
        if (along < 28) t = d < HALF ? T_CHECKER : T_SAND;                            /* start / finish line */
        else if (d < 3) t = ((int)(along / 48) & 1) ? T_DASH : T_ASPHALT;            /* centre dashes */
        else if (d < HALF - LINE_W) t = T_ASPHALT;
        else if (d < HALF) t = T_LINE;
        else t = ((int)(along / 40) & 1) ? T_KERB_RED : T_KERB_WHITE;
        m->cells[idx] = t;
    }
    free(dist); free(near);
    if (SDL_getenv("SABER_M7MAP")) {   /* debug: dump the material map */
        FILE *f = fopen(SDL_getenv("SABER_M7MAP"), "wb");
        if (f) { fprintf(f, "P5\n%d %d\n255\n", MAPN, MAPN); for (int i = 0; i < MAPN * MAPN; i++) fputc(m->cells[i] * 20, f); fclose(f); }
    }
}

/* nearest track sample to (x,y) starting the search from a hint; returns s, lateral (signed, +right of travel) */
static int track_project(const Mode7 *m, float x, float y, int hint, float *s_out, float *lat_out, float *tang_out)
{
    int best = hint; float bd = 1e30f;
    int span = hint < 0 ? TRACK_N : 40;
    for (int k = -span; k <= span; k++) {
        int i = ((hint < 0 ? 0 : hint) + k + TRACK_N * 4) % TRACK_N;
        float d = dwrap(x, m->tx[i]) * dwrap(x, m->tx[i]) + dwrap(y, m->ty[i]) * dwrap(y, m->ty[i]);
        if (d < bd) { bd = d; best = i; }
        if (hint < 0 && k >= TRACK_N - 1) break;
    }
    int nx = (best + 1) % TRACK_N;
    float tx = m->tx[nx] - m->tx[best], ty = m->ty[nx] - m->ty[best]; float tl = hypotf(tx, ty); tx /= tl; ty /= tl;
    float rx = dwrap(x, m->tx[best]), ry = dwrap(y, m->ty[best]);
    float along = rx * tx + ry * ty;
    if (s_out) { *s_out = m->tlen[best] + along; if (*s_out < 0) *s_out += m->track_len; if (*s_out >= m->track_len) *s_out -= m->track_len; }
    if (lat_out) *lat_out = rx * -ty + ry * tx;   /* right of travel: (-ty, tx) */
    if (tang_out) *tang_out = atan2f(ty, tx);
    return best;
}
static void track_point(const Mode7 *m, float s, float lat, float *x, float *y, float *heading)
{
    s = fmodf(s, m->track_len); if (s < 0) s += m->track_len;
    int i = (int)(s / m->track_len * TRACK_N) % TRACK_N;   /* samples are nearly uniform */
    while (i > 0 && m->tlen[i] > s) i--;
    while (i < TRACK_N - 1 && m->tlen[i + 1] <= s) i++;
    int n = (i + 1) % TRACK_N;
    float seg = (n ? m->tlen[n] : m->track_len) - m->tlen[i]; float t = seg > 0 ? (s - m->tlen[i]) / seg : 0;
    float tx = m->tx[n] - m->tx[i], ty = m->ty[n] - m->ty[i]; float tl = hypotf(tx, ty); tx /= tl; ty /= tl;
    *x = m->tx[i] + tx * seg * t + (-ty) * lat; *y = m->ty[i] + ty * seg * t + tx * lat;
    if (heading) *heading = atan2f(ty, tx);
}

static void place_props_around_track(Mode7 *m)
{
    for (int i = 0; i < 90; i++) {
        float s = frand(m) * m->track_len, side = frand(m) < 0.5f ? -1 : 1, lat = side * (170 + frand(m) * 400);
        float x, y; track_point(m, s, lat, &x, &y, NULL);
        if (is_road(cell_at(m, x, y)) || is_rumble(cell_at(m, x, y))) continue;
        Ent *e = ent_new(m); if (!e) break;
        float r = frand(m);
        e->kind = K_PROP; e->spr = r < 0.45f ? S_CACTUS : r < 0.8f ? S_ROCK_S : r < 0.95f ? S_ROCK_B : S_MESA; e->x = wrapf(x); e->y = wrapf(y);
        e->scale = e->spr == S_MESA ? 2.2f : e->spr == S_ROCK_B ? 1.5f : 1.2f; e->solid = true;
    }
    /* the finish gate */
    Ent *g = ent_new(m);
    if (g) { g->kind = K_GATE; g->spr = S_GATE; g->x = m->tx[0]; g->y = m->ty[0]; g->scale = 3.9f; }   /* 160 px x 3.9 x CAM_BACK / FOCAL = the road's 208 units plus the kerbs */
}

/* the open desert with the dirt road north to Dome City down its middle (x = WORLD / 2), soft shoulders */
static void build_desert(Mode7 *m)
{
    fill_sand(m);
    const int ROAD = 108 >> MAPSH, SHOULDER = 128 >> MAPSH, mid = MAPN / 2;
    for (int cy = 0; cy < MAPN; cy++) {
        int wob = (int)(sinf(cy * 0.021f) * 3 + sinf(cy * 0.0071f) * 5);   /* the road meanders a little */
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
    place_props_around_track(m);
    /* the grid: 8 cars, two abreast, behind the line */
    static const struct { int spr; float max; bool hornet; const char *name; } FIELD[N_RACERS] = {
        { S_FIRENZA, 625, false, "FIRENZA" }, { S_HORNET, 605, true, "HORNET" }, { S_HORNET, 590, true, "HORNET" },
        { S_RBLUE, 570, false, "VEGA" }, { S_HORNET, 575, true, "HORNET" }, { S_RPURPLE, 555, false, "KELLY" }, { S_RBLUE, 540, false, "DUNN" },
    };
    for (int i = 0; i < N_RACERS; i++) {
        Ent *e = ent_new(m); if (!e) break;
        e->kind = K_RACER; e->spr = FIELD[i].spr; e->max_speed = FIELD[i].max; e->hornet = FIELD[i].hornet; e->id = i;
        e->s = m->track_len - 60.0f * (i + 1) - 30; e->lat = (i & 1) ? 48 : -48; e->lat_target = e->lat; e->solid = true;
        e->lap = -1;   /* the grid sits behind the line: progress (lap * track_len + s) is the distance past it, like the player's */
        e->hp = e->hp_max = (e->hornet ? 10 : 7) + 2 * m->difficulty;
        track_point(m, e->s, e->lat, &e->x, &e->y, &e->heading); e->speed = 0;
    }
    /* player last on the grid (right column) */
    m->s = m->track_len - 60.0f * (N_RACERS + 1) - 30; m->lat = 48; m->lap = 0; m->progress = m->s - m->track_len;   /* behind the line: the first crossing is lap 1 */
    track_point(m, m->s, m->lat, &m->px, &m->py, &m->heading); m->cam_heading = m->heading; m->speed = 0; m->last_s = m->s;
    m->near_idx = -1; m->race_time = 0;
}

Mode7 *mode7_create(SDL_Renderer *ren, int sw, int sh, int difficulty, int lives)
{
    Mode7 *m = calloc(1, sizeof *m);
    m->ren = ren; m->sw = sw; m->sh = sh; m->rng = 0xC0FFEE;
    m->floor_h = sh - HORIZON - 1;
    m->floor_px = calloc((size_t)sw * m->floor_h, 4);
    m->floor_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, sw, m->floor_h);
    SDL_SetTextureScaleMode(m->floor_tex, SDL_SCALEMODE_NEAREST);
    m->ok = load_atlas(m);
    if (m->ok && m->spr[S_BUGGY].frames < 5) { fprintf(stderr, "mode7.png is stale (buggy needs 5 steering frames): rerun tools/build_mode7_assets.py\n"); m->ok = false; }
    m->sky_ok = load_sky(m);
    m->difficulty = difficulty; m->lives = lives;
    m->max_hp = difficulty == 0 ? 16 : difficulty == 1 ? 12 : 8; m->hp = m->max_hp;   /* the car's damage meter: shots 1, mines / crashes 2 */
    m->boost = 1.0f; m->boost_locked = false; m->music_now = -1;
    dialog_set_hero(HERO_FIREBALL);   /* the Grand Prix is Fireball's story whoever was picked: everyone rides in his buggy */
    start_race(m);
    m->phase = PH_INTRO; m->phase_t = 0;
    play_music(m, 10, true);
    m->intro_pending = true;
    if (SDL_getenv("SABER_M7PHASE")) {   /* debug: 1 race (no story), 2 pursuit, 3 boss, 4 the finish -> briefing */
        int ph = atoi(SDL_getenv("SABER_M7PHASE")); m->intro_pending = false;
        if (ph == 1) { m->phase = PH_COUNTDOWN; m->countdown = 3.99f; if (SDL_getenv("SABER_M7LAP")) { m->lap = 1; m->progress = m->track_len + m->s; for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_RACER) m->ents[i].lap = 1; } }
        else if (ph == 2) { m->phase = PH_PURSUIT; begin_pursuit(m); }
        else if (ph == 3) { begin_pursuit(m); m->phase = PH_BOSS; begin_boss(m); if (SDL_getenv("SABER_M7BOSSHP")) m->ents[m->boss_i].hp = (float)atof(SDL_getenv("SABER_M7BOSSHP")); }
        else if (ph == 4) { m->phase = PH_RACE; m->lap = 2; m->progress = 2 * m->track_len + m->s; m->last_s = m->s; m->speed = 500; for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_RACER) { m->ents[i].lap = 2; m->ents[i].speed = 500; } }   /* the grid, two laps in: 510 units to the flag */
    }
    return m;
}

void mode7_destroy(Mode7 *m)
{
    if (!m) return;
    sfx_loop(NULL);
    if (m->atlas) SDL_DestroyTexture(m->atlas);
    if (m->floor_tex) SDL_DestroyTexture(m->floor_tex);
    if (m->sky_tex) SDL_DestroyTexture(m->sky_tex);
    free(m->floor_px); free(m);
}

int mode7_result(const Mode7 *m) { return m->result; }
int mode7_lives(const Mode7 *m) { return m->lives; }

/* ---------------------------------------------------------------- combat helpers */
static bool phase_plays(const Mode7 *m) { return m->phase == PH_RACE || m->phase == PH_PURSUIT || m->phase == PH_BOSS; }

static void player_hurt(Mode7 *m, int dmg)
{
    if (m->hurt_t > 0 || !phase_plays(m)) return;   /* no damage while a story scene or the countdown holds the car */
    m->hp -= dmg; m->hurt_t = 0.7f; m->shake = 0.4f; sfx_play(3, 0);
    if (m->hp <= 0) {
        m->hp = 0; spawn_expl(m, m->px, m->py, 1.6f); sfx_play(5, 0); sfx_play(6, 3); sfx_loop(NULL); m->turbo_on = false;
        m->phase_t = 0; m->dead_t = 0; m->resume_phase = m->phase;
        m->phase = PH_DEAD; m->speed = 0;
    }
}

static void fire_shot(Mode7 *m, float x, float y, float heading, float speed, bool player_owned, float z)
{
    Ent *e = ent_new(m); if (!e) return;
    e->kind = player_owned ? K_SHOT : K_ESHOT; e->spr = player_owned ? S_SHOT : S_ESHOT;
    e->x = x; e->y = y; e->z = z; e->heading = heading; e->speed = speed; e->player_owned = player_owned; e->t = player_owned ? 1.1f : 2.2f;
    e->vx = cosf(heading) * speed; e->vy = sinf(heading) * speed;
    if (player_owned) { Ent *f = ent_new(m); if (f) { f->kind = K_FLASH; f->spr = S_FLASH; f->x = x; f->y = y; f->z = z; f->t = 0.08f; } }
}

/* enemy guns are deliberately sloppy: a shot goes at the player with this much random spread (radians) */
static float aim_at_player(Mode7 *m, const Ent *e, float spread) { return atan2f(dwrap(m->py, e->y), dwrap(m->px, e->x)) + (frand(m) - 0.5f) * spread; }

static void damage_ent(Mode7 *m, Ent *e, float dmg)
{
    if (e->kind == K_BOSS && e->state == B_DYING) return;
    e->hp -= dmg; e->knock = 0.25f; sfx_play(14, 0);
    if (e->hp <= 0) {
        if (e->kind == K_BOSS) {   /* the leader burns out over a couple of seconds before he blows (update_ents) */
            e->state = B_DYING; e->t = 2.4f; e->t2 = 0; e->hp = 0; m->kills++;
            spawn_expl(m, e->x, e->y, 1.2f); sfx_play(5, 0); set_msg(m, "HORNET LEADER DOWN", 2.0f);
            return;
        }
        spawn_expl(m, e->x, e->y, 1.2f);
        sfx_play(5, 0); sfx_play(6, 3); m->kills++;
        if (e->kind == K_RACER || e->kind == K_ESCORT) set_msg(m, e->hornet ? "BLACK HORNET DESTROYED" : "RIVAL WRECKED", 2.0f);
        e->kind = K_NONE;
    }
}

/* the leader's Hornet escort: runs north ahead of the player and drops back to block and shoot */
static void spawn_escorts(Mode7 *m, int n)
{
    for (int k = 0; k < n; k++) {
        Ent *e = ent_new(m); if (!e) break;
        e->kind = K_ESCORT; e->spr = S_HORNET; e->hornet = true; e->hp = e->hp_max = 4 + m->difficulty; e->scale = 1.0f; e->solid = true;
        e->x = wrapf(WORLD * 0.5f + (frand(m) - 0.5f) * 160); e->y = wrapf(m->py - 900 - frand(m) * 300); e->state = 0; e->t = 1.0f + frand(m) * 2; e->heading = -PI / 2; e->speed = 300;
    }
}

/* ---------------------------------------------------------------- update: the player's car */
static void player_drive(Mode7 *m, const Input *in, float dt, bool free_drive)
{
    uint8_t tile = cell_at(m, m->px, m->py);
    bool road = is_road(tile), rumble = is_rumble(tile);
    float vmax = road ? 470.0f : rumble ? 380.0f : 250.0f;
    if (m->boost_locked && m->boost >= 0.5f) m->boost_locked = false;   /* recharged halfway: turbo usable again */
    bool turbo = !m->boost_locked && btn_down(in, BTN_AIM) && m->boost > 0.05f && m->spin_t <= 0 && phase_plays(m);
    if (!free_drive && SDL_getenv("SABER_M7AUTO") && atoi(SDL_getenv("SABER_M7AUTO")) >= 2 && m->spin_t <= 0 && phase_plays(m))   /* debug: the auto-driver also uses the turbo */
        turbo = !m->boost_locked && (m->turbo_on ? m->boost > 0.05f : m->boost > 0.6f);
    if (turbo) {
        vmax *= 1.35f; m->boost -= dt * 0.33f;
        /* used up: locked out until halfway. The floor is 0.05 (the engage gate below), not 0: otherwise a held
         * button hovers just under 0.05, flickering turbo without ever locking */
        if (m->boost <= 0.05f) { m->boost = 0; m->boost_locked = true; }
    }
    else { m->boost = clampf(m->boost + dt * 0.08f, 0, 1); if (m->boost_locked && m->boost >= 0.5f) m->boost_locked = false; }
    if (turbo && !m->turbo_on) { sfx_play_file(asset_path("sfx/turbo_start.wav")); sfx_loop(asset_path("sfx/turbo_loop.wav")); }
    else if (!turbo && m->turbo_on) sfx_loop(NULL);
    m->turbo_on = turbo; if (turbo) m->turbo_t += dt;
    /* slipstream: tucked in close behind a rival, the car tows along a little faster */
    if (!free_drive && !turbo) for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &m->ents[i]; if (e->kind != K_RACER) continue;
        float gap = e->lap * m->track_len + e->s - m->progress;
        if (gap > 20 && gap < 260 && fabsf(e->lat - m->lat) < 40) { vmax *= 1.12f; break; }
    }
    bool accel = btn_down(in, BTN_JUMP) || btn_down(in, BTN_UP);
    bool brake = btn_down(in, BTN_DOWN);
    float steer = (btn_down(in, BTN_LEFT) ? -1 : 0) + (btn_down(in, BTN_RIGHT) ? 1 : 0);
    if (!free_drive && SDL_getenv("SABER_M7AUTO")) {   /* debug: drive along the circuit (test laps without a driver) */
        float ax, ay; track_point(m, m->s + 260, 0, &ax, &ay, NULL);
        float want = atan2f(dwrap(ay, m->py), dwrap(ax, m->px)), d = angdiff(want, m->heading);
        steer = d > 0.05f ? 1 : d < -0.05f ? -1 : 0; accel = true;
    }
    if (m->spin_t > 0) { accel = false; m->spin_t -= dt; }
    if (accel) m->speed += (turbo ? 520.0f : 300.0f) * dt;
    else m->speed -= 120.0f * dt;
    if (brake) m->speed -= 500.0f * dt;
    if (m->speed > vmax) m->speed -= (m->speed - vmax) * (road ? 6.0f : 8.0f) * dt;
    if (m->speed < 0) m->speed = brake ? clampf(m->speed, -80, 0) : 0;
    if (m->spin_t > 0) steer = 0;
    float turn = 1.9f * steer * clampf(fabsf(m->speed) / 300.0f, 0, 1.15f);
    if (!road && !rumble) turn *= 0.85f;
    m->heading += turn * dt;
    m->tilt += (steer * 7.0f - m->tilt) * clampf(dt * 8, 0, 1);
    if (m->spin_t > 0) m->heading += 6.0f * dt;
    m->vx = cosf(m->heading) * m->speed; m->vy = sinf(m->heading) * m->speed;
    m->px = wrapf(m->px + m->vx * dt); m->py = wrapf(m->py + m->vy * dt);
    /* camera heading lags a touch behind the car for the drifting feel */
    m->cam_heading += angdiff(m->heading, m->cam_heading) * clampf(dt * 9, 0, 1);
    m->anim_t += fabsf(m->speed) * dt * 0.02f;
    if (rumble) m->bounce = 1.5f * ((int)(m->anim_t * 8) & 1); else m->bounce = (!road && m->speed > 100) ? (float)((int)(m->anim_t * 6) & 1) : 0;
    if (m->fire_cd > 0) m->fire_cd -= dt;
    if (btn_down(in, BTN_SHOOT) && m->fire_cd <= 0 && m->spin_t <= 0) {
        m->fire_cd = 0.16f; sfx_play(1, 0);
        fire_shot(m, m->px + cosf(m->heading) * 20, m->py + sinf(m->heading) * 20, m->heading, 1500.0f + fabsf(m->speed), true, 14);
    }
    if (!free_drive) {
        float s; m->near_idx = track_project(m, m->px, m->py, m->near_idx, &s, &m->lat, NULL);
        float ds = s - m->last_s;
        if (ds < -m->track_len * 0.5f) { ds += m->track_len; }
        else if (ds > m->track_len * 0.5f) { ds -= m->track_len; }
        m->progress += ds; m->last_s = s; m->s = s;
        int lap = (int)floorf(m->progress / m->track_len); if (lap < 0) lap = 0;   /* a lap counts under the gate (s = 0), not a track length after the grid */
        if (lap > m->lap) { m->lap = lap; set_msg(m, lap == 1 ? "LAP 2" : lap == 2 ? "FINAL LAP" : "FINISH", 2.0f); }
    }
}

/* ---------------------------------------------------------------- update: entities */
static void update_racers(Mode7 *m, float dt)
{
    /* a light rubber band around the player's progress: the pack stays reachable, but a leader is only reeled in
     * with turbo and the slipstream (the player settles at ~520 on the road, ~720 on turbo) */
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &m->ents[i]; if (e->kind != K_RACER) continue;
        float prog = e->lap * m->track_len + e->s, gap = prog - m->progress;
        float target = e->max_speed;
        if (gap > 2600) target *= 0.75f; else if (gap > 1200) target *= 0.86f; else if (gap < -900) target *= 1.12f;
        if (e->knock > 0) { e->knock -= dt; target *= 0.5f; }
        if (m->phase == PH_COUNTDOWN || m->phase == PH_INTRO || m->phase == PH_INSTRUCTIONS) target = 0;
        if (m->phase == PH_FINISH || m->phase == PH_BREAKAWAY) target = e->hornet ? 760 : target * 0.8f;   /* the Hornets bolt, the field winds down */
        e->speed += (target - e->speed) * clampf(dt * (e->speed < target ? 1.6f : 2.5f), 0, 1);
        /* racing line wobble; a car just ahead of the player drifts over to block the pass. The grid holds still
         * (no line changes, no steering pose) until the lights go out */
        bool grid = m->phase == PH_COUNTDOWN || m->phase == PH_INTRO || m->phase == PH_INSTRUCTIONS;
        if (!grid) {
            e->t -= dt;
            if (e->t <= 0) { e->t = 1.5f + frand(m) * 3; e->lat_target = (frand(m) - 0.5f) * 150; }
            if (gap > 30 && gap < 320 && m->phase == PH_RACE) e->lat_target += (m->lat - e->lat_target) * clampf(dt * (e->hornet ? 1.6f : 0.8f), 0, 1);
            e->lat += (e->lat_target - e->lat) * clampf(dt * 1.2f, 0, 1);
        } else { e->lat_target = e->lat; e->tilt = 0; }
        float prev_s = e->s; e->s += e->speed * dt;
        if (e->s >= m->track_len) { e->s -= m->track_len; e->lap++; }
        (void)prev_s;
        track_point(m, e->s, e->lat, &e->x, &e->y, &e->heading);
        e->tilt += ((e->lat_target - e->lat) * 0.05f - e->tilt) * clampf(dt * 5, 0, 1);   /* steering pose from where it is heading */
        /* Black Hornets fight: mines behind when ahead of the player, shots when behind */
        if (e->hornet && m->phase == PH_RACE && m->race_time > 8.0f && e->speed > 200) {
            e->t2 -= dt;
            if (e->t2 <= 0) {
                if (gap > 60 && gap < 900) { e->t2 = 1.8f + frand(m) * 1.5f; Ent *mn = ent_new(m); if (mn) { mn->kind = K_MINE; mn->spr = S_MINE; mn->x = e->x; mn->y = e->y; mn->t = 12; } }
                else if (gap < -40 && gap > -800) { e->t2 = 1.2f + frand(m); fire_shot(m, e->x, e->y, aim_at_player(m, e, 0.5f), 650, false, 12); sfx_play(7, 0); }
                else e->t2 = 0.5f;
            }
        }
    }
}

static void bump_player(Mode7 *m, Ent *e, float dist, float r)
{
    float nx = dwrap(m->px, e->x), ny = dwrap(m->py, e->y); if (dist < 1) { nx = 1; ny = 0; dist = 1; }
    nx /= dist; ny /= dist; float push = (r - dist) * 0.6f;
    m->px = wrapf(m->px + nx * push); m->py = wrapf(m->py + ny * push);
    if (e->kind == K_RACER) { e->lat += (e->lat > m->lat ? 1 : -1) * push * 0.6f; e->speed *= 0.9f; m->speed *= 0.9f; m->shake = 0.15f; }
}

static void update_ents(Mode7 *m, float dt)
{
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &m->ents[i]; if (e->kind == K_NONE) continue;
        switch (e->kind) {
        case K_SHOT: case K_ESHOT:
            e->x = wrapf(e->x + e->vx * dt); e->y = wrapf(e->y + e->vy * dt); e->t -= dt;
            if (e->t <= 0) { e->kind = K_NONE; break; }
            if (e->kind == K_SHOT) {
                for (int j = 0; j < MAX_ENT; j++) {
                    Ent *o = &m->ents[j];
                    if (o->kind != K_RACER && o->kind != K_ESCORT && o->kind != K_BOSS && o->kind != K_MINE) continue;
                    float r = o->kind == K_BOSS ? 42 : o->kind == K_MINE ? 18 : 34;
                    if (fabsf(dwrap(e->x, o->x)) < r && fabsf(dwrap(e->y, o->y)) < r) {
                        e->kind = K_NONE;
                        if (o->kind == K_MINE) { spawn_expl(m, o->x, o->y, 0.8f); o->kind = K_NONE; sfx_play(5, 0); }
                        else if (o->kind == K_BOSS && o->state == B_FLEE) { o->knock = 0.3f; sfx_play(14, 0); }   /* a hit slows the fleeing leader: shooting helps close the gap */
                        else damage_ent(m, o, 1);
                        break;
                    }
                }
            } else if (fabsf(dwrap(e->x, m->px)) < 22 && fabsf(dwrap(e->y, m->py)) < 22) { e->kind = K_NONE; player_hurt(m, 1); }   /* a hit on the car */
            break;
        case K_FLASH: e->t -= dt; if (e->t <= 0) e->kind = K_NONE; break;
        case K_EXPL: e->t += dt; e->frame = (int)(e->t * 14); if (e->frame >= 6) e->kind = K_NONE; if (e->frame == 2 && e->t2 == 0) { e->t2 = 1; spawn_smoke(m, e->x, e->y, 20 * e->scale); } break;
        case K_SMOKE: e->t += dt; e->frame = (int)(e->t * 6); e->z += 30 * dt; if (e->frame >= 4) e->kind = K_NONE; break;
        case K_MINE:
            e->t -= dt; e->anim += dt; e->frame = ((int)(e->anim * 4)) & 1;
            if (e->t <= 0) { e->kind = K_NONE; break; }
            if (fabsf(dwrap(e->x, m->px)) < 26 && fabsf(dwrap(e->y, m->py)) < 26) { spawn_expl(m, e->x, e->y, 1.0f); e->kind = K_NONE; player_hurt(m, 2); m->speed *= 0.4f; m->spin_t = 0.6f; }
            break;
        case K_PROP: {
            float dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y); float r = e->spr == S_CACTUS ? 18 : e->spr == S_MESA ? 70 : e->spr == S_ROCK_B ? 40 : 24;
            float d = hypotf(dx, dy);
            if (d < r + 16) {
                float nx = dx / (d > 1 ? d : 1), ny = dy / (d > 1 ? d : 1);
                m->px = wrapf(e->x + nx * (r + 17)); m->py = wrapf(e->y + ny * (r + 17));
                if (m->speed > 150 && m->hurt_t <= 0) { player_hurt(m, 2); m->spin_t = 0.5f; }
                m->speed = -fabsf(m->speed) * 0.35f - 40; m->shake = 0.2f;   /* bounce off */
            }
            break; }
        case K_RACER: {
            float dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y); float d = hypotf(dx, dy);
            if (d < 44) bump_player(m, e, d, 44);
            break; }
        case K_ESCORT: {
            /* the leader's Hornet escort: runs north ahead of the player, weaving, and drops back to block and shoot */
            e->t -= dt;
            float dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y), d = hypotf(dx, dy);
            float prev_h = e->heading;
            if (e->state == 0) {   /* running, weaving across the road */
                e->heading = -PI / 2 + sinf(e->anim) * 0.35f; e->anim += dt * 1.3f;
                e->speed += (300 - e->speed) * clampf(dt * 2, 0, 1);
                if (e->t <= 0) { e->state = 1; e->t = 2.5f + frand(m) * 1.5f; }
            } else {               /* slowed right down, guns on the player */
                e->speed += (90 - e->speed) * clampf(dt * 3, 0, 1); e->t2 -= dt;
                e->heading = -PI / 2 + sinf(e->anim * 3) * 0.1f;
                if (e->t2 <= 0 && d < 1400) { e->t2 = 1.1f; fire_shot(m, e->x, e->y, aim_at_player(m, e, 0.45f), 600, false, 12); sfx_play(7, 0); }
                if (e->t <= 0) { e->state = 0; e->t = 3 + frand(m) * 3; }
            }
            e->tilt += (angdiff(e->heading, prev_h) * 60 - e->tilt) * clampf(dt * 6, 0, 1);
            if (e->knock > 0) { e->knock -= dt; e->speed *= 0.97f; }
            e->x = wrapf(e->x + cosf(e->heading) * e->speed * dt); e->y = wrapf(e->y + sinf(e->heading) * e->speed * dt);
            if (d < 44) { bump_player(m, e, d, 44); if (m->hurt_t <= 0) player_hurt(m, 1); m->speed *= 0.6f; }
            /* fell too far behind the player (he passed it): drop it */
            if (dy < -1800 || dy > 5000) e->kind = K_NONE;
            break; }
        case K_BOSS: {
            e->t -= dt; if (e->knock > 0) e->knock -= dt;
            float dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y), d = hypotf(dx, dy);
            float prev_h = e->heading;
            float gap = dy;   /* how far north of the player he is (dy = player y - his y, y grows southward) */
            if (e->state == B_DYING) {   /* burning out: rolls to a stop, sparks and smoke, then the big one */
                e->speed += (0 - e->speed) * clampf(dt * 1.2f, 0, 1);
                e->t2 -= dt;
                if (e->t2 <= 0) { e->t2 = 0.16f; spawn_expl(m, e->x + (frand(m) - 0.5f) * 50, e->y + (frand(m) - 0.5f) * 50, 0.6f + frand(m) * 0.5f); spawn_smoke(m, e->x, e->y, 30); if (frand(m) < 0.5f) sfx_play(14, 0); }
                e->heading += sinf(e->t * 9) * 0.6f * dt;   /* fishtailing */
                if (e->t <= 0) {
                    spawn_expl(m, e->x, e->y, 2.6f); spawn_expl(m, e->x + 30, e->y - 20, 1.6f); spawn_expl(m, e->x - 30, e->y + 20, 1.6f);
                    sfx_play(5, 0); sfx_play(6, 3); sfx_play(14, 6); m->shake = 0.6f;
                    m->wreck_x = e->x; m->wreck_y = e->y; m->wreck_t = 0;
                    e->kind = K_NONE; break;
                }
            } else if (e->state == B_FLEE) {   /* the pursuit: up the road, weaving, pace rubber-banded to the gap so he stays in reach but never free */
                float target = gap > 3400 ? 250 : gap > 2200 ? 380 : gap < 600 ? 540 : 470;   /* the player does ~550 on the road, ~700 on turbo */
                if (e->knock > 0 && e->t <= 0) target *= 0.75f;
                if (e->t <= 0 && gap < 560 && gap > 0 && m->pursuit_t < 40 && e->t < -3) {   /* early in the chase he always has one more booster (e->t: > 0 boosting, < 0 seconds since) */
                    e->t = 3.5f; e->speed = 700; set_msg(m, "THE HORNET HITS HIS BOOSTER", 1.5f); sfx_play(0x13, 0);
                    for (int k = 0; k < 3; k++) spawn_smoke(m, e->x - cosf(e->heading) * 30 * k, e->y - sinf(e->heading) * 30 * k, 10);
                }
                if (e->t > 0) target = 700;   /* booster */
                e->speed += (target - e->speed) * clampf(dt * (e->t > 0 ? 4.0f : 1.5f), 0, 1);
                e->anim += dt * 0.9f;
                float road_x = WORLD * 0.5f + sinf(e->anim) * 60;
                e->heading = -PI / 2 + clampf(dwrap(road_x, e->x) * 0.004f, -0.4f, 0.4f);
                e->t2 -= dt;
                if (e->t2 <= 0 && gap < 900 && gap > 0) {   /* mines out the back when the player is close */
                    e->t2 = 1.4f + frand(m); Ent *mn = ent_new(m); if (mn) { mn->kind = K_MINE; mn->spr = S_MINE; mn->x = e->x; mn->y = e->y; mn->t = 14; }
                }
            } else {   /* B_RUN, the fight (Chase H.Q.): he keeps racing up the road just ahead of you, weaving to block, mines out the
                        * back, a rear gunner, a booster now and then; you shoot him and ram him. Pace rubber-banded to the gap */
                float target = gap < 0 ? 560 : gap < 140 ? 600 : gap < 520 ? 445 : gap < 1200 ? 360 : 280;   /* the player does ~470 on the dirt, ~630 on turbo; passed, he re-passes */
                if (e->knock > 0) target *= 0.8f;
                e->t3 -= dt;
                if (e->t3 <= 0 && gap > 0 && gap < 450 && e->t <= 0) {   /* booster: opens the gap again for a couple of seconds */
                    e->t = 2.3f; e->t3 = 8 + frand(m) * 4; e->speed = 700; set_msg(m, "THE HORNET HITS HIS BOOSTER", 1.5f); sfx_play(0x13, 0);
                    for (int k = 0; k < 3; k++) spawn_smoke(m, e->x - cosf(e->heading) * 30 * k, e->y - sinf(e->heading) * 30 * k, 10);
                }
                if (e->t > 0) target = 700;
                e->speed += (target - e->speed) * clampf(dt * (e->t > 0 ? 4.0f : 1.6f), 0, 1);
                e->anim += dt * 1.4f;
                float road_x = WORLD * 0.5f + sinf(e->anim) * 85;
                if (gap > 0 && gap < 260) road_x -= clampf(dwrap(m->px, e->x), -60, 60);   /* jinks out of your line when you close in */
                e->heading = -PI / 2 + clampf(dwrap(road_x, e->x) * 0.006f, -0.5f, 0.5f);
                e->t2 -= dt;
                if (e->t2 <= 0 && gap > 60 && gap < 700) {   /* mines out the back */
                    e->t2 = 1.0f + frand(m) * 0.8f; Ent *mn = ent_new(m); if (mn) { mn->kind = K_MINE; mn->spr = S_MINE; mn->x = e->x + (frand(m) - 0.5f) * 40; mn->y = e->y; mn->t = 16; }
                }
                e->gun_t -= dt;
                if (e->gun_t <= 0 && gap > 90 && gap < 1500) {   /* the rear gunner: sloppy, so the shots can be dodged */
                    e->gun_t = 1.2f + frand(m) * 0.6f; fire_shot(m, e->x, e->y, aim_at_player(m, e, 0.4f), 600, false, 16); sfx_play(7, 0);
                }
            }
            e->tilt += (angdiff(e->heading, prev_h) * 40 - e->tilt) * clampf(dt * 6, 0, 1);
            e->x = wrapf(e->x + cosf(e->heading) * e->speed * dt); e->y = wrapf(e->y + sinf(e->heading) * e->speed * dt);
            if (d < 70 && e->state != B_DYING) {   /* contact: a fast ram from behind dents him (the Chase H.Q. way), a side-swipe just costs you speed */
                bump_player(m, e, d, 70);
                if (m->ram_cd <= 0) {
                    m->ram_cd = 0.5f;
                    if (m->speed > 330 && gap > 0) { damage_ent(m, e, 4); m->speed *= 0.55f; m->shake = 0.35f; set_msg(m, "RAM!", 0.6f); }
                    else { m->speed *= 0.6f; m->shake = 0.15f; }
                }
            }
            if (e->state == B_RUN && e->hp < e->hp_max * 0.4f && ((int)(m->phase_t * 6) & 1)) spawn_smoke(m, e->x, e->y, 40);
            break; }
        default: break;
        }
    }
}

/* the placing: one more than the cars further past the start line than the player */
static void standings(Mode7 *m)
{
    int ahead = 0;
    for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_RACER && m->ents[i].lap * m->track_len + m->ents[i].s > m->progress) ahead++;
    m->rank = ahead + 1;
}

/* ---------------------------------------------------------------- phases */
/* the chase: the Hornet leader runs north up the desert road with the player on his tail; catch him to start the fight */
static void begin_pursuit(Mode7 *m)
{
    ents_clear(m, false);
    build_desert(m);
    m->px = WORLD * 0.5f; m->py = WORLD * 0.5f; m->heading = m->cam_heading = -PI / 2; m->speed = 200;
    m->pursuit_spawn_t = 2.5f; m->pursuit_t = 0;
    for (int i = 0; i < 160; i++) {
        Ent *e = ent_new(m); if (!e) break;
        float x = frand(m) * WORLD, y = frand(m) * WORLD;
        if (fabsf(dwrap(x, WORLD * 0.5f)) < 190) { e->kind = K_NONE; continue; }
        float r = frand(m);
        e->kind = K_PROP; e->spr = r < 0.5f ? S_CACTUS : r < 0.85f ? S_ROCK_S : r < 0.96f ? S_ROCK_B : S_MESA; e->x = x; e->y = y;
        e->scale = e->spr == S_MESA ? 2.2f : e->spr == S_ROCK_B ? 1.5f : 1.2f; e->solid = true;
    }
    Ent *b = ent_new(m);
    b->kind = K_BOSS; b->spr = S_LEADER; b->scale = 1.25f; b->hp = b->hp_max = m->boss_hp_max = m->difficulty == 0 ? 60 : m->difficulty == 1 ? 80 : 100;
    b->x = WORLD * 0.5f; b->y = m->py - 1500; b->state = B_FLEE; b->heading = -PI / 2; b->speed = 300; b->t = -10; b->t2 = 3; b->solid = true;
    m->boss_i = (int)(b - m->ents); m->gap = 1500;
    play_music(m, 14, true);
    set_msg(m, "CATCH THE HORNET LEADER", 3.0f);
}

/* caught him: the fight is on, but he keeps racing up the road (B_RUN) */
static void begin_boss(Mode7 *m)
{
    Ent *b = &m->ents[m->boss_i];
    b->state = B_RUN; b->t = 0; b->t2 = 1.5f; b->t3 = 5; b->knock = 0; b->anim = 0;
    for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_ESCORT || m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE;
    m->pursuit_spawn_t = 7.0f;
    play_music(m, 17, true);
}

static void begin_victory(Mode7 *m)
{
    m->phase = PH_VICTORY; m->phase_t = 0;   /* the car coasts to a stop by the burning wreck */
    if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "victory after %.0f s\n", m->phase_t);
    for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE;
    music_play(6, false); m->music_now = 6;
    dialog_open_script(&m->dlg, SCRIPT_VICTORY);
}

void mode7_update(Mode7 *m, const Input *in, float dt)
{
    if (!m->ok) { m->result = 1; return; }
    if (m->result) return;
    if (btn_pressed(in, BTN_PAUSE) && (m->phase == PH_RACE || m->phase == PH_PURSUIT || m->phase == PH_BOSS)) {
        m->paused = !m->paused; sfx_play(10, 0); music_pause(m->paused); if (m->paused) { sfx_loop(NULL); m->turbo_on = false; }
    }
    if (m->paused) return;
    { static int kill = -2; if (kill == -2) kill = SDL_getenv("SABER_KILL") ? atoi(SDL_getenv("SABER_KILL")) : -1; if (kill >= 0 && kill-- == 0) { m->hurt_t = 0; player_hurt(m, 99); } }   /* debug: die at step N */
    m->phase_t += dt;
    { static int last = -1, step; step++; if (SDL_getenv("SABER_TRACE") && m->phase != last) { fprintf(stderr, "m7 phase %d at step %d\n", m->phase, step); last = m->phase; } }
    if (m->msg_t > 0) m->msg_t -= dt;
    if (m->hurt_t > 0) m->hurt_t -= dt;
    if (m->shake > 0) m->shake -= dt;
    if (m->ram_cd > 0) m->ram_cd -= dt;
    Input idle = { 0 }; for (int b = 0; b < BTN_COUNT; b++) idle.state[b] = 1;

    switch (m->phase) {
    case PH_INTRO:
        if (m->intro_pending) { m->intro_pending = false; dialog_open_script(&m->dlg, SCRIPT_INTRO); }
        if (m->dlg.active) dialog_update(&m->dlg, in, dt);
        else { m->phase = PH_INSTRUCTIONS; m->phase_t = 0; }
        break;
    case PH_INSTRUCTIONS: {
        bool any = false; for (int b = 0; b < BTN_COUNT; b++) if (btn_pressed(in, b)) any = true;
        if (any && m->phase_t >= INSTR_OPEN_DUR) m->phase_t = INSTR_MIN_DUR;   /* skip: jump straight to the close */
        if (m->phase_t >= INSTR_MIN_DUR) { m->phase = PH_COUNTDOWN; m->phase_t = 0; m->countdown = 3.99f; }
        break; }
    case PH_COUNTDOWN: {
        int before = (int)m->countdown; m->countdown -= dt; int after = (int)m->countdown;
        if (after != before) sfx_play(0, 0);
        update_racers(m, dt); standings(m);
        if (m->countdown <= 1.0f) { m->phase = PH_RACE; m->phase_t = 0; set_msg(m, "GO!", 1.0f); sfx_play(8, 0); }
        break; }
    case PH_RACE:
        m->race_time += dt;
        player_drive(m, in, dt, false);
        if (SDL_getenv("SABER_TRACE") && (int)m->race_time != (int)(m->race_time - dt)) fprintf(stderr, "race t=%.0f lap=%d s=%.0f lat=%.0f v=%.0f rank=%d hp=%d turbo=%d boost=%.2f\n", m->race_time, m->lap, m->s, m->lat, m->speed, m->rank, m->hp, m->turbo_on, m->boost);
        update_racers(m, dt); update_ents(m, dt);
        standings(m);
        if (m->lap >= 3) {   /* the chequered flag after three laps: the car coasts on under the FINISH banner while the
                              * Hornets bolt off the course */
            m->phase = PH_FINISH; m->phase_t = 0; m->finish_rank = m->rank; m->msg_t = 0; sfx_loop(NULL); m->turbo_on = false;
            music_stop(); m->music_now = -1;
            for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_RACER && m->ents[i].hornet) { m->ents[i].lat_target = 420; m->ents[i].t = 99; }   /* off the course, no more line changes */
            for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE;
        }
        break;
    case PH_FINISH: {
        /* hands off the wheel: the car follows the track and eases off, the field rolls on past the flag */
        Input coast = { 0 }; for (int b = 0; b < BTN_COUNT; b++) coast.state[b] = 1;
        float ax, ay; track_point(m, m->s + 240, 0, &ax, &ay, NULL);
        float want = atan2f(dwrap(ay, m->py), dwrap(ax, m->px)), d = angdiff(want, m->heading);
        coast.state[BTN_LEFT] = d < -0.05f ? 0 : 1; coast.state[BTN_RIGHT] = d > 0.05f ? 0 : 1;
        coast.state[BTN_UP] = m->phase_t < 1.2f ? 0 : 1;
        player_drive(m, &coast, dt, false);
        update_racers(m, dt); update_ents(m, dt);
        if (m->phase_t >= FINISH_DUR) {
            if (m->finish_rank <= 3) { m->phase = PH_BREAKAWAY; m->phase_t = 0; dialog_open_script(&m->dlg, SCRIPT_BREAKAWAY); play_music(m, 14, true); }
            else if (m->lives <= 0) { m->phase = PH_GAMEOVER; m->phase_t = 0; music_set_volume(0.5f); }   /* must rank 3rd or better: no spares left */
            else {   /* must rank 3rd or better: lose a life and start the race over from the beginning */
                m->lives--;
                start_race(m);
                m->hp = m->max_hp; m->hurt_t = 0; m->spin_t = 0; m->boost = 1; m->boost_locked = false; m->speed = 0; m->turbo_on = false; m->shake = 0; m->tilt = 0; m->fire_cd = 0; m->turbo_t = 0;
                m->phase = PH_COUNTDOWN; m->phase_t = 0; m->countdown = 3.99f;
                play_music(m, 10, true);
                set_msg(m, "QUALIFY 3RD OR BETTER", 3.0f);
            }
        }
        break; }
    case PH_BREAKAWAY: {
        Input coast = { 0 }; for (int b = 0; b < BTN_COUNT; b++) coast.state[b] = 1;   /* the car rolls on under the radio call */
        float ax, ay; track_point(m, m->s + 240, 0, &ax, &ay, NULL);
        float d = angdiff(atan2f(dwrap(ay, m->py), dwrap(ax, m->px)), m->heading);
        coast.state[BTN_LEFT] = d < -0.05f ? 0 : 1; coast.state[BTN_RIGHT] = d > 0.05f ? 0 : 1;
        player_drive(m, &coast, dt, false);
        update_racers(m, dt); update_ents(m, dt);
        if (m->dlg.active) dialog_update(&m->dlg, in, dt);
        else { m->phase = PH_BRIEF; m->phase_t = 0; music_stop(); m->music_now = -1; sfx_play(0x13, 0); }
        break; }
    case PH_BRIEF: {   /* the Chase H.Q. target card: the lines type in, a button skips to the zoom, the zoom hands over */
        bool any = false; for (int b = 0; b < BTN_COUNT; b++) if (btn_pressed(in, b)) any = true;
        int line_prev = (int)((m->phase_t - dt) * 2.2f), line_now = (int)(m->phase_t * 2.2f);
        if (line_now != line_prev && m->phase_t < BRIEF_TEXT_DUR - 0.6f) sfx_play(0, 0);   /* a blip per line */
        if (any && m->phase_t > 0.3f + 5 / 2.2f + 0.3f && m->phase_t < BRIEF_TEXT_DUR) m->phase_t = BRIEF_TEXT_DUR;   /* once the lines are in */
        if (m->phase_t >= BRIEF_TEXT_DUR + BRIEF_ZOOM_DUR) { m->phase = PH_PURSUIT; m->phase_t = 0; m->white = 1; begin_pursuit(m); }
        break; }
    case PH_PURSUIT: {
        if (m->white > 0) m->white = clampf(m->white - dt / PURSUIT_FADE, 0, 1);
        player_drive(m, in, dt, true);
        update_ents(m, dt);
        m->pursuit_t += dt;
        Ent *b = &m->ents[m->boss_i];
        m->gap = hypotf(dwrap(b->x, m->px), dwrap(b->y, m->py));
        if (SDL_getenv("SABER_TRACE") && (int)m->pursuit_t != (int)(m->pursuit_t - dt)) fprintf(stderr, "pursuit t=%.0f gap=%.0f leader %.0f,%.0f v=%.0f | player %.0f,%.0f v=%.0f\n", m->pursuit_t, m->gap, b->x, b->y, b->speed, m->px, m->py, m->speed);
        /* the escort drops back in pairs to get between the player and the leader */
        m->pursuit_spawn_t -= dt;
        if (m->pursuit_spawn_t <= 0 && m->gap > 700) {
            m->pursuit_spawn_t = 3.0f + frand(m) * 2.0f;
            spawn_escorts(m, 1 + (frand(m) < 0.4f ? 1 : 0) + (m->difficulty == 2 ? 1 : 0));
        }
        if (m->gap < CATCH_GAP && dwrap(b->y, m->py) < 0) {   /* on his tail: he stops running */
            m->phase = PH_CAUGHT; m->phase_t = 0; m->speed *= 0.5f;
            dialog_open_script(&m->dlg, SCRIPT_CAUGHT);
        }
        break; }
    case PH_CAUGHT:
        if (m->dlg.active) dialog_update(&m->dlg, in, dt);
        else { m->phase = PH_BOSS; m->phase_t = 0; begin_boss(m); set_msg(m, "DESTROY THE HORNET LEADER", 3.0f); }
        break;
    case PH_BOSS: {
        player_drive(m, in, dt, true);
        update_ents(m, dt);
        Ent *b = &m->ents[m->boss_i];
        if (b->kind != K_BOSS) { begin_victory(m); break; }
        m->gap = dwrap(b->y, m->py) < 0 ? hypotf(dwrap(b->x, m->px), dwrap(b->y, m->py)) : 0;
        m->pursuit_spawn_t -= dt;   /* the odd escort still comes back to block */
        if (m->pursuit_spawn_t <= 0 && b->state == B_RUN) { m->pursuit_spawn_t = 7.0f + frand(m) * 4.0f; spawn_escorts(m, 1); }
        if (SDL_getenv("SABER_TRACE") && ((int)(m->phase_t * 60) % 60) == 0) fprintf(stderr, "boss st=%d pos=%.0f,%.0f hp=%.0f | player %.0f,%.0f h=%.2f hp=%d\n", b->state, b->x, b->y, b->hp, m->px, m->py, m->heading, m->hp);
        break; }
    case PH_VICTORY:
        if (m->speed > 0) idle.state[BTN_DOWN] = 0;   /* brakes on until it stands */
        player_drive(m, &idle, dt, true); update_ents(m, dt);
        m->wreck_t -= dt;
        if (m->wreck_t <= 0) { m->wreck_t = 0.3f; spawn_smoke(m, m->wreck_x + (frand(m) - 0.5f) * 40, m->wreck_y + (frand(m) - 0.5f) * 40, 10); if (frand(m) < 0.3f) spawn_expl(m, m->wreck_x + (frand(m) - 0.5f) * 40, m->wreck_y + (frand(m) - 0.5f) * 40, 0.5f); }
        if (m->phase_t > 2.5f) { if (m->dlg.active) dialog_update(&m->dlg, in, dt); else if (m->phase_t > 3.0f) { m->phase = PH_CLEARED; m->phase_t = 0; } }
        break;
    case PH_CLEARED:
        if (m->phase_t > 1.2f) m->result = 1;
        break;
    case PH_DEAD:
        update_ents(m, dt);
        if (m->phase_t > 2.5f) {
            if (m->lives <= 0) { m->phase = PH_GAMEOVER; m->phase_t = 0; music_set_volume(0.5f); }
            else {
                m->lives--; m->hp = m->max_hp; m->hurt_t = 2.0f; m->speed = 0; m->spin_t = 0; m->boost = 1; m->boost_locked = false;
                /* back onto the course / the road */
                if (m->resume_phase == PH_BOSS) { Ent *b = &m->ents[m->boss_i]; m->px = WORLD * 0.5f; m->py = wrapf(b->y + 500); m->heading = m->cam_heading = -PI / 2; m->phase = PH_BOSS; b->t = 0; b->speed = 200; for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_ESCORT || m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE; }
                else if (m->resume_phase == PH_PURSUIT) { Ent *b = &m->ents[m->boss_i]; m->px = WORLD * 0.5f; m->py = wrapf(b->y + 1200); m->heading = m->cam_heading = -PI / 2; m->phase = PH_PURSUIT; for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_ESCORT || m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE; }
                else { track_point(m, m->s, 0, &m->px, &m->py, &m->heading); m->cam_heading = m->heading; m->lat = 0; m->phase = PH_RACE; for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE; }
            }
            m->phase_t = 0;
        }
        break;
    case PH_GAMEOVER:
        if (m->phase_t > 1.5f) m->result = 2;
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- drawing */
static void render_floor(Mode7 *m)
{
    float ch = m->cam_heading;
    float fx = cosf(ch), fy = sinf(ch), rx = -fy, ry = fx;
    float camx = m->px - fx * CAM_BACK, camy = m->py - fy * CAM_BACK;
    int sw = m->sw;
    const uint32_t haze = 0xFF8CB2D8u;   /* ABGR (R d8, G b2, B 8c): dust at the horizon */
    for (int row = 0; row < m->floor_h; row++) {
        int y = HORIZON + 1 + row;
        float d = CAM_H * FOCAL / (float)(y - HORIZON);
        float step = d / FOCAL;   /* world units per screen pixel across this row */
        float fog = clampf((d - FOG0) / (FOG1 - FOG0), 0, 1);
        int fa = (int)(fog * 256);
        int mip = step < 1.5f ? 0 : step < 3.0f ? 1 : step < 6.0f ? 2 : 3;
        int msz = TEX >> mip, mmask = msz - 1, msh = 5 - mip;
        float wx = camx + fx * d - rx * step * (sw * 0.5f), wy = camy + fy * d - ry * step * (sw * 0.5f);
        uint32_t *out = m->floor_px + (size_t)row * sw;
        for (int x = 0; x < sw; x++) {
            int ix = (int)floorf(wx), iy = (int)floorf(wy);
            uint8_t t = m->cells[((iy >> MAPSH) & (MAPN - 1)) * MAPN + ((ix >> MAPSH) & (MAPN - 1))];
            uint32_t c = m->tiles[t][mip][(((iy >> mip) & mmask) << msh) + ((ix >> mip) & mmask)];
            if (fa) {
                uint32_t r = ((c & 0xff) * (256 - fa) + (haze & 0xff) * fa) >> 8;
                uint32_t g = (((c >> 8) & 0xff) * (256 - fa) + ((haze >> 8) & 0xff) * fa) >> 8;
                uint32_t b = (((c >> 16) & 0xff) * (256 - fa) + ((haze >> 16) & 0xff) * fa) >> 8;
                c = 0xff000000u | b << 16 | g << 8 | r;
            }
            out[x] = c;
            wx += rx * step; wy += ry * step;
        }
    }
    SDL_UpdateTexture(m->floor_tex, NULL, m->floor_px, sw * 4);
    SDL_FRect dst = { 0, (float)(HORIZON + 1), (float)sw, (float)m->floor_h };
    SDL_RenderTexture(m->ren, m->floor_tex, NULL, &dst);
}

static void render_horizon(Mode7 *m)
{
    SDL_SetRenderDrawColor(m->ren, 78, 160, 214, 255);
    SDL_FRect sky = { 0, 0, (float)m->sw, (float)(HORIZON + 1) }; SDL_RenderFillRect(m->ren, &sky);
    if (!m->sky_ok) return;
    /* the panorama is scaled to fill the sky band exactly (no squash: both axes share one factor), then tiled at
     * its own width - it was authored to loop there, so this is a clean wrap with none of the dead columns /
     * narrow-slice repeats that reusing level 1's background layers had */
    float sc = (float)(HORIZON + 1) / m->sky_h, tw = m->sky_w * sc;
    float turn = m->cam_heading / TWO_PI;
    float ox = fmodf(turn * tw, tw); if (ox < 0) ox += tw;
    for (float x = -ox; x < m->sw; x += tw) {
        SDL_FRect dst = { x, 0, tw, (float)(HORIZON + 1) };
        SDL_RenderTexture(m->ren, m->sky_tex, NULL, &dst);
    }
}

/* the buggy's nozzles drift a few px sideways in the hard steering poses of the clip */
static float steer_frame_shift(int frame) { return frame == 0 ? -3 : frame == 1 ? -1 : frame == 3 ? 1 : frame == 4 ? 3 : 0; }
/* which of a car's three steering poses (left / straight / right) to show */
static int steer_frame(float tilt) { return tilt < -0.5f ? 0 : tilt > 0.5f ? 2 : 1; }

typedef struct { float d, sx, sy, scale; Ent *e; } DrawItem;
static int cmp_far(const void *a, const void *b) { float x = ((const DrawItem *)a)->d, y = ((const DrawItem *)b)->d; return x < y ? 1 : x > y ? -1 : 0; }

static void render_sprites(Mode7 *m)
{
    float ch = m->cam_heading;
    float fx = cosf(ch), fy = sinf(ch), rx = -fy, ry = fx;
    float camx = m->px - fx * CAM_BACK, camy = m->py - fy * CAM_BACK;
    DrawItem items[MAX_ENT]; int n = 0;
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &m->ents[i]; if (e->kind == K_NONE) continue;
        float dx = dwrap(e->x, camx), dy = dwrap(e->y, camy);
        float d = dx * fx + dy * fy, lat = dx * rx + dy * ry;
        if (d < 12 || d > 4200) continue;
        float sx = m->sw * 0.5f + lat * FOCAL / d;
        float sy = HORIZON + CAM_H * FOCAL / d - e->z * FOCAL / d;
        float scale = CAM_BACK / d * e->scale;
        if (sx < -200 || sx > m->sw + 200) continue;
        items[n++] = (DrawItem){ d, sx, sy, scale, e };
    }
    qsort(items, n, sizeof *items, cmp_far);
    for (int i = 0; i < n; i++) {
        Ent *e = items[i].e; float fog = clampf((items[i].d - FOG0) / (FOG1 - FOG0), 0, 1);
        uint8_t a = (uint8_t)(255 * (1 - fog * 0.85f));
        uint8_t r = e->r, g = e->g, b = e->b;
        if (e->knock > 0 && ((int)(e->knock * 30) & 1)) { r = 255; g = 120; b = 120; }
        int frame = e->frame;
        if (e->kind == K_RACER || e->kind == K_ESCORT || e->kind == K_BOSS) frame = steer_frame(e->tilt + angdiff(e->heading, m->cam_heading) * 2);
        draw_spr(m, e->spr, frame, items[i].sx, items[i].sy, items[i].scale, 0, r, g, b, a);
        if ((e->kind == K_RACER || e->kind == K_ESCORT) && items[i].d < 2600 && e->hp_max > 0) {   /* a small health bar over every rival */
            float w = 20 * clampf(items[i].scale * 1.6f, 0.5f, 1.5f), top = items[i].sy - m->spr[e->spr].h * items[i].scale - 5;
            float f = e->hp / e->hp_max;
            SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(m->ren, 0, 0, 0, 150); SDL_FRect bg = { floorf(items[i].sx - w * 0.5f - 1), floorf(top - 1), w + 2, 4 }; SDL_RenderFillRect(m->ren, &bg);
            SDL_SetRenderDrawColor(m->ren, f > 0.5f ? 90 : 240, f > 0.25f ? 220 : 80, 60, 255); SDL_FRect fg = { floorf(items[i].sx - w * 0.5f), floorf(top), w * f, 2 }; SDL_RenderFillRect(m->ren, &fg);
        }
        if (e->kind == K_BOSS && e->state != B_DYING) {   /* the target: red corner brackets around the leader, pulsing */
            float hw = m->spr[e->spr].w * items[i].scale * 0.5f + 4 + 2 * ((int)(m->phase_t * 6) & 1), hh = m->spr[e->spr].h * items[i].scale + 8;
            float x0 = floorf(items[i].sx - hw), x1 = floorf(items[i].sx + hw), y1 = floorf(items[i].sy + 4), y0 = floorf(y1 - hh);
            float L = clampf(hw * 0.4f, 3, 10);
            SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_NONE); SDL_SetRenderDrawColor(m->ren, 255, 50, 50, 255);
            SDL_FRect q[8] = { { x0, y0, L, 2 }, { x0, y0, 2, L }, { x1 - L, y0, L, 2 }, { x1 - 2, y0, 2, L },
                               { x0, y1 - 2, L, 2 }, { x0, y1 - L, 2, L }, { x1 - L, y1 - 2, L, 2 }, { x1 - 2, y1 - L, 2, L } };
            SDL_RenderFillRects(m->ren, q, 8);
        }
    }
}

static void render_player(Mode7 *m)
{
    if (m->phase == PH_DEAD || m->phase == PH_GAMEOVER) return;   /* the wreck stays gone */
    float sy = HORIZON + CAM_H * FOCAL / CAM_BACK - 6 - m->bounce;
    float sx = m->sw * 0.5f + m->tilt * 1.2f;
    if (m->shake > 0) { sx += (float)(rand() % 5 - 2); sy += (float)(rand() % 3 - 1); }
    /* the clip is a steering set: hard left, left, straight, right, hard right */
    int frame = m->tilt < -5 ? 0 : m->tilt < -1.5f ? 1 : m->tilt <= 1.5f ? 2 : m->tilt <= 5 ? 3 : 4;
    uint8_t r = 255, g = 255, b = 255;
    if (m->hurt_t > 0 && ((int)(m->hurt_t * 20) & 1)) { r = 255; g = 90; b = 90; }
    float ang = m->spin_t > 0 ? m->spin_t * 720 : 0;
    draw_spr(m, S_BUGGY, frame, sx, sy, 1, ang, r, g, b, 255);
    if (m->turbo_on && ang == 0) {   /* the afterburner: a flame over each exhaust nozzle (10x10 at (23,21) and (60,21) of the sprite) */
        Spr *bs = &m->spr[S_BUGGY]; float x0 = floorf(sx - bs->w * 0.5f), y0 = floorf(sy - bs->h);
        int fr = (int)(m->turbo_t * 18) & 3;
        static const float NOZ[2][2] = { { 23, 21 }, { 60, 21 } };
        for (int k = 0; k < 2; k++) {
            float fx = x0 + NOZ[k][0] + 5 + (steer_frame_shift(frame)) - 6.0f, fy = y0 + NOZ[k][1] + 10;   /* -6px: the flames sat right of the nozzles */
            draw_spr(m, S_TURBO, fr, fx, fy, 1, 0, 255, 255, 255, 255);
            draw_spr(m, S_TURBO, (fr + 2) & 3, fx, fy + 1, 1.4f, 0, 255, 255, 255, 110);   /* a soft halo behind it */
        }
    }
}

static void bar(SDL_Renderer *r, float x, float y, float w, float h, float f, uint8_t R, uint8_t G, uint8_t B)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 160); SDL_FRect bg = { x - 1, y - 1, w + 2, h + 2 }; SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, R, G, B, 255); SDL_FRect fg = { x, y, w * clampf(f, 0, 1), h }; SDL_RenderFillRect(r, &fg);
}

/* the chequered flag: a band of black / white squares rolls across the middle of the screen with FINISH on it, the
 * placing under it */
static void render_finish(Mode7 *m, Font *f, Font *small)
{
    float t = m->phase_t; int sw = m->sw;
    float in = clampf(t / 0.35f, 0, 1); in = 1 - (1 - in) * (1 - in);
    float out = clampf((t - (FINISH_DUR - 0.4f)) / 0.4f, 0, 1);
    float bx = -sw * (1 - in) + sw * out * out;   /* rolls in from the left, leaves to the right */
    const int CS = 10; float y0 = 64;
    SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_NONE);
    for (int row = 0; row < 2; row++) for (int col = -1; col <= sw / CS + 1; col++) {
        int scroll = (int)(t * 60) / CS;
        bool white = ((col + row + scroll) & 1) == 0;
        SDL_SetRenderDrawColor(m->ren, white ? 245 : 20, white ? 245 : 20, white ? 245 : 24, 255);
        SDL_FRect q = { bx + col * CS, y0 + row * CS, CS, CS }; SDL_RenderFillRect(m->ren, &q);
        SDL_FRect q2 = { bx + col * CS, y0 + 52 + row * CS, CS, CS }; SDL_RenderFillRect(m->ren, &q2);
    }
    SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(m->ren, 0, 0, 0, 170);
    SDL_FRect mid = { bx, y0 + 20, (float)sw, 32 }; SDL_RenderFillRect(m->ren, &mid);
    const char *fin = "FINISH!"; float fw = (float)font_text_width(f, fin);
    font_draw(f, fin, bx + sw * 0.5f - fw * 0.5f + 1, y0 + 25, 40, 30, 0);
    font_draw(f, fin, bx + sw * 0.5f - fw * 0.5f, y0 + 24, 255, 210, 40);
    static const char *const ORD[] = { "1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH" };
    char buf[32]; int rk = m->finish_rank < 1 ? 1 : m->finish_rank > 8 ? 8 : m->finish_rank;
    snprintf(buf, sizeof buf, "%s PLACE", ORD[rk - 1]);
    if (t > 0.8f) font_draw(small, buf, bx + sw * 0.5f - font_text_width(small, buf) * 0.5f, y0 + 40, 255, 255, 255);
    if (t > 1.6f && ((int)(t * 3) & 1)) {
        const char *w = rk <= 3 ? "THE HORNETS ARE LEAVING THE COURSE!" : "MUST FINISH 3RD OR BETTER!";
        font_draw(small, w, sw * 0.5f - font_text_width(small, w) * 0.5f, 140, 255, 90, 90);
    }
}

/* the Chase H.Q. target briefing: a black card, a blue console panel whose lines type in one by one with the
 * Hornet leader's car in a pulsing red reticle at the right; at the end the target zooms into the camera under a
 * white-out and the pursuit fades in from that white (m->white, PH_PURSUIT) */
static void render_brief(Mode7 *m, Font *f, Font *small)
{
    float t = m->phase_t; int sw = m->sw, sh = m->sh;
    SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(m->ren, 0, 0, 0, 255); SDL_FRect all = { 0, 0, (float)sw, (float)sh }; SDL_RenderFillRect(m->ren, &all);
    /* the panel wipes open from the middle in the first 0.3 s */
    float open = clampf(t / 0.3f, 0, 1); open = 1 - (1 - open) * (1 - open);
    bool narrow = sw < 400;   /* 4:3: no room beside the lines, so the panel grows and the target sits under them */
    float ph = (narrow ? 200 : 150) * open, py = (sh - ph) * 0.5f;
    SDL_SetRenderDrawColor(m->ren, 10, 18, 44, 255); SDL_FRect panel = { 16, py, (float)(sw - 32), ph }; SDL_RenderFillRect(m->ren, &panel);
    SDL_SetRenderDrawColor(m->ren, 60, 120, 220, 255); SDL_FRect top = { 16, py - 2, (float)(sw - 32), 2 }, bot = { 16, py + ph, (float)(sw - 32), 2 }; SDL_RenderFillRect(m->ren, &top); SDL_RenderFillRect(m->ren, &bot);
    SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(m->ren, 60, 120, 220, 40);
    for (float y = py; y < py + ph; y += 3) { SDL_FRect ln = { 16, y, (float)(sw - 32), 1 }; SDL_RenderFillRect(m->ren, &ln); }   /* console scanlines */
    if (open < 1) return;
    /* the lines, typed in at 2.2 lines / s */
    static const char *const LINES[] = { "CAVALRY COMMAND - ALERT", "TARGET:  BLACK HORNET LEADER", "VEHICLE: HORNET RACING BUGGY", "HEADING: NORTH - DOME CITY", "ORDERS:  PURSUE AND DESTROY" };
    int nl = (int)(sizeof LINES / sizeof *LINES); float tl = t - 0.3f;
    for (int i = 0; i < nl; i++) {
        float lt = tl - i / 2.2f; if (lt < 0) break;
        int len = (int)strlen(LINES[i]), shown = (int)(lt * 40); if (shown > len) shown = len;
        uint8_t r = i == 0 ? 255 : i == nl - 1 ? 255 : 200, g = i == 0 ? 182 : i == nl - 1 ? 90 : 220, b = i == 0 ? 0 : i == nl - 1 ? 90 : 255;
        font_draw_n(small, LINES[i], shown, 30, py + 12 + i * 16, r, g, b);
        if (shown < len && ((int)(t * 12) & 1)) { SDL_SetRenderDrawColor(m->ren, 200, 220, 255, 255); SDL_FRect cur = { 30 + font_text_width_n(small, LINES[i], shown), py + 12 + i * 16, 6, (float)small->h }; SDL_RenderFillRect(m->ren, &cur); }
    }
    if (tl > nl / 2.2f + 0.3f) {
        const char *go = t < BRIEF_TEXT_DUR ? "PRESS A BUTTON" : "GO!";
        if (((int)(t * 4) & 1) || t >= BRIEF_TEXT_DUR) font_draw(f, go, 30, py + ph - 26, 255, 255, 255);
    }
    /* the target: the leader's car in the reticle, then the zoom */
    float zt = clampf((t - BRIEF_TEXT_DUR) / BRIEF_ZOOM_DUR, 0, 1), zz = zt * zt * zt;
    float sc0 = narrow ? 1.0f : 1.6f, cx0 = narrow ? sw - 80 : sw - 78, cy0 = narrow ? py + ph - 12 : py + ph * 0.5f + 28;
    float cx = cx0 + (sw * 0.5f - cx0) * zt, cy = cy0 + (sh * 0.5f + 60 - cy0) * zt;
    float sc = sc0 + 14.0f * zz;
    /* a pulsing red reticle around the car's silhouette, the scan bar rolling down it */
    Spr *ls = &m->spr[S_LEADER]; float hw = ls->w * sc * 0.5f + 6 + 2 * ((int)(t * 6) & 1), hh = ls->h * sc + 10;
    SDL_SetRenderDrawColor(m->ren, 60, 20, 30, 255); SDL_FRect bg = { cx - hw, cy - hh + 2, hw * 2, hh }; SDL_RenderFillRect(m->ren, &bg);
    draw_spr(m, S_LEADER, 1, cx, cy, sc, 0, 255, 255, 255, 255);
    float x0 = cx - hw, x1 = cx + hw, y1 = cy + 4, yy0 = y1 - hh, L = clampf(hw * 0.4f, 4, 14);
    SDL_SetRenderDrawColor(m->ren, 255, 50, 50, 255);
    SDL_FRect q[8] = { { x0, yy0, L, 2 }, { x0, yy0, 2, L }, { x1 - L, yy0, L, 2 }, { x1 - 2, yy0, 2, L }, { x0, y1 - 2, L, 2 }, { x0, y1 - L, 2, L }, { x1 - L, y1 - 2, L, 2 }, { x1 - 2, y1 - L, 2, L } };
    SDL_RenderFillRects(m->ren, q, 8);
    if (zt == 0) { SDL_SetRenderDrawColor(m->ren, 255, 80, 80, 120); SDL_FRect scan = { x0, yy0 + fmodf(t * 40, hh), hw * 2, 2 }; SDL_RenderFillRect(m->ren, &scan); }
    if (tl > 1.0f && zt == 0) font_draw(small, "TARGET", cx - font_text_width(small, "TARGET") * 0.5f, yy0 - 12, 255, 60, 60);
    if (zt > 0) { SDL_SetRenderDrawColor(m->ren, 255, 255, 255, (uint8_t)(255 * zz)); SDL_RenderFillRect(m->ren, &all); }
}

static void render_hud(Mode7 *m)
{
    Font *f = font_get(0x4058897F), *small = font_get(0x12072E60);
    if (!f || !small) return;
    char buf[64];
    /* the car: damage meter + spare cars (it is Fireball's buggy whoever drives, so no hero portrait / hearts here) */
    {
        float hp = clampf((float)m->hp / m->max_hp, 0, 1);
        font_draw(small, "RED FURY", 8, 6, 255, 182, 0);
        bar(m->ren, 8, 16, 72, 6, hp, hp > 0.5f ? 90 : 240, hp > 0.25f ? 220 : 80, 60);
        if (m->hurt_t > 0 && ((int)(m->hurt_t * 20) & 1)) bar(m->ren, 8, 16, 72, 6, 1, 255, 255, 255);
        draw_spr(m, S_BUGGY, 2, 18, 40, 0.28f, 0, 255, 255, 255, 255);
        snprintf(buf, sizeof buf, "x%d", m->lives);
        font_draw(small, buf, 30, 30, 255, 255, 255);
    }
    /* turbo meter + speed (hidden under a dialog box, which sits on the same rows and covers them in 4:3) */
    if (!m->dlg.active) {
        if (m->boost_locked) bar(m->ren, 8, m->sh - 14, 60, 5, m->boost, 230, 50, 50);   /* used up: red until halfway */
        else bar(m->ren, 8, m->sh - 14, 60, 5, m->boost, 255, 182, 0);
        font_draw(small, "TURBO", 8, (float)(m->sh - 26), 255, 182, 0);
        snprintf(buf, sizeof buf, "%3d", (int)(fabsf(m->speed) * 0.6f));
        font_draw(f, buf, (float)(m->sw - 8 - font_text_width(f, buf)), (float)(m->sh - 20), 255, 255, 255);
        font_draw(small, "KM/H", (float)(m->sw - 12 - font_text_width(small, "KM/H")), (float)(m->sh - 30), 200, 200, 200);
    }
    if (m->phase == PH_RACE || m->phase == PH_COUNTDOWN || m->phase == PH_FINISH || m->phase == PH_BREAKAWAY) {
        static const char *const ORD[] = { "1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH" };
        snprintf(buf, sizeof buf, "LAP %d/3", m->lap + 1 > 3 ? 3 : m->lap + 1);
        font_draw(f, buf, (float)(m->sw - 8 - font_text_width(f, buf)), 8, 255, 255, 255);
        int rk = m->rank < 1 ? 1 : m->rank > 8 ? 8 : m->rank;
        font_draw(f, ORD[rk - 1], (float)(m->sw - 8 - font_text_width(f, ORD[rk - 1])), 24, rk == 1 ? 255 : 255, rk == 1 ? 182 : 255, rk == 1 ? 0 : 255);
        snprintf(buf, sizeof buf, "%d'%02d\"%02d", (int)m->race_time / 60, (int)m->race_time % 60, (int)(m->race_time * 100) % 100);
        font_draw(small, buf, (float)(m->sw / 2 - font_text_width(small, buf) / 2), 8, 255, 255, 255);
        if (m->phase == PH_COUNTDOWN) {
            int c = (int)m->countdown; if (c >= 1 && c <= 3) { snprintf(buf, sizeof buf, "%d", c); font_draw(f, buf, (float)(m->sw / 2 - font_text_width(f, buf) / 2), 70, 255, 60, 60); }
        }
    } else if (m->phase == PH_PURSUIT || m->phase == PH_CAUGHT || m->phase == PH_BOSS) {
        Ent *b = &m->ents[m->boss_i];
        font_draw(small, "HORNET LEADER", (float)(m->sw / 2 - font_text_width(small, "HORNET LEADER") / 2), 6, 255, 120, 120);
        if (m->phase == PH_PURSUIT) {   /* how close you are to catching him */
            bar(m->ren, (float)(m->sw / 2 - 70), 18, 140, 5, 1.0f - clampf((m->gap - CATCH_GAP) / (GAP_MAX - CATCH_GAP), 0, 1), 255, 182, 0);
            snprintf(buf, sizeof buf, "GAP %4dM", (int)m->gap);
            font_draw(small, buf, (float)(m->sw - 8 - font_text_width(small, buf)), 8, 255, 255, 255);
        } else bar(m->ren, (float)(m->sw / 2 - 70), 18, 140, 5, b->kind == K_BOSS ? b->hp / m->boss_hp_max : 0, 230, 50, 50);
        if (b->kind == K_BOSS) {   /* where is he? a red arrow along the screen edge when the leader is off screen */
            float rel = angdiff(atan2f(dwrap(b->y, m->py), dwrap(b->x, m->px)), m->cam_heading);
            if (fabsf(rel) > 0.75f) {
                float cx = m->sw * 0.5f + sinf(rel) * (m->sw * 0.45f), cy = 60 - cosf(rel) * 40 + 60;
                SDL_SetRenderDrawColor(m->ren, 255, 60, 60, 255);
                SDL_FRect q = { cx - 4, cy - 4, 8, 8 }; SDL_RenderFillRect(m->ren, &q);
                font_draw(small, rel > 0 ? ">" : "<", cx + (rel > 0 ? 6 : -12), cy - 5, 255, 60, 60);
            }
        }
    }
    if (m->msg_t > 0) font_draw(f, m->msg, (float)(m->sw / 2 - font_text_width(f, m->msg) / 2), 96, 255, 255, 255);
    if (m->phase == PH_FINISH) render_finish(m, f, small);
    if (m->paused) {
        SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(m->ren, 0, 0, 0, 64);
        SDL_FRect q = { 0, 0, (float)m->sw, (float)m->sh }; SDL_RenderFillRect(m->ren, &q);
        Sprite *ps = sprite_get(0xB2143E42);
        if (ps && ((SDL_GetTicks() / 16) & 0x7f) > 0x30) sprite_draw(ps, 0, (float)((m->sw - ps->w) / 2), (float)((m->sh - ps->h) / 2), false);
    }
}

/* the controls card: the grid scene dims behind it while the panel wipes open from the middle (matches
 * render_brief's grow), sized off sw/sh so it reads the same in 16:9 and 4:3; any button skips it once open,
 * and it times out on its own after INSTR_MIN_DUR so nothing can get stuck on it */
static void render_instructions(Mode7 *m, Font *f, Font *small)
{
    float t = m->phase_t; int sw = m->sw, sh = m->sh;
    float open = clampf(t / INSTR_OPEN_DUR, 0, 1); open = 1 - (1 - open) * (1 - open);
    SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m->ren, 0, 0, 0, (uint8_t)(140 * open));
    SDL_FRect scrim = { 0, 0, (float)sw, (float)sh }; SDL_RenderFillRect(m->ren, &scrim);
    static const struct { const char *label, *desc; } LINES[] = {
        { "STEER", "Left / Right" }, { "ACCELERATE", "Jump button or Up" }, { "FIRE", "Shoot button" },
        { "TURBO", "Aim button" }, { "BRAKE", "Down" },
    };
    int nl = (int)(sizeof LINES / sizeof *LINES);
    float full_h = 34 + nl * 14 + 22;
    float pw = (float)sw - 40, ph = full_h * open, px0 = 20, py0 = (sh - full_h) * 0.5f + (full_h - ph) * 0.5f;
    SDL_SetRenderDrawColor(m->ren, 10, 18, 44, (uint8_t)(255 * open));
    SDL_FRect panel = { px0, py0, pw, ph }; SDL_RenderFillRect(m->ren, &panel);
    SDL_SetRenderDrawColor(m->ren, 60, 120, 220, (uint8_t)(255 * open));
    SDL_FRect top = { px0, py0 - 2, pw, 2 }, bot = { px0, py0 + ph, pw, 2 };
    SDL_RenderFillRect(m->ren, &top); SDL_RenderFillRect(m->ren, &bot);
    if (open < 1 || !f || !small) return;
    const char *title = "ALL GALAXY GRAND PRIX";
    font_draw(f, title, px0 + (pw - font_text_width(f, title)) * 0.5f, py0 + 8, 255, 182, 0);
    for (int i = 0; i < nl; i++) {
        float ly = py0 + 30 + i * 14;
        font_draw(small, LINES[i].label, px0 + 14, ly, 255, 224, 192);
        font_draw(small, LINES[i].desc, px0 + 110, ly, 220, 230, 255);
    }
    const char *sub = "THREE LAPS!";
    font_draw(small, sub, px0 + (pw - font_text_width(small, sub)) * 0.5f, py0 + full_h - 26, 255, 255, 255);
    if (t >= INSTR_OPEN_DUR && ((int)(t * 4) & 1)) {
        const char *skip = "PRESS A BUTTON TO SKIP";
        font_draw(small, skip, px0 + (pw - font_text_width(small, skip)) * 0.5f, py0 + full_h - 12, 200, 200, 200);
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
    if (m->white > 0) { SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(m->ren, 255, 255, 255, (uint8_t)(255 * m->white)); SDL_FRect q = { 0, 0, (float)m->sw, (float)m->sh }; SDL_RenderFillRect(m->ren, &q); }
    if (scanlines) {
        SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(m->ren, 0, 0, 0, 70);
        for (int y = 1; y < m->sh; y += 2) { SDL_FRect q = { 0, (float)y, (float)m->sw, 1 }; SDL_RenderFillRect(m->ren, &q); }
    }
    if (m->phase == PH_GAMEOVER || m->phase == PH_CLEARED) {
        float a = clampf(m->phase_t / (m->phase == PH_CLEARED ? 1.2f : 1.5f), 0, 1); uint8_t v = m->phase == PH_CLEARED ? 255 : 0;
        SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(m->ren, v, v, v, (uint8_t)(a * 255));
        SDL_FRect q = { 0, 0, (float)m->sw, (float)m->sh }; SDL_RenderFillRect(m->ren, &q);
    }
}
