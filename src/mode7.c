#include "mode7.h"
#include "assets.h"
#include "gfx.h"
#include "level.h"
#include "font.h"
#include "audio.h"
#include "hud.h"
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
#define WN 256               /* cells per side (wraps) */
#define CELL 32
#define WORLD (WN * CELL)
enum { T_SAND, T_SAND2, T_ASPHALT, T_ASPHALT_L, T_ASPHALT_R, T_RUMBLE_A, T_RUMBLE_B, T_CHECKER, T_DIRT, T_SAND_DARK, T_ASPHALT_DASH, T_PLAZA, T_COUNT };

/* ---- atlas ---- */
enum { S_BUGGY, S_HORNET, S_FIRENZA, S_RBLUE, S_RPURPLE, S_TANK, S_APRIL, S_FIREBALL, S_CACTUS, S_ROCK_S, S_ROCK_B, S_MESA,
       S_FLOOR, S_DOME, S_SHOT, S_ESHOT, S_EXPL, S_FLASH, S_MINE, S_GATE, S_SMOKE, S_CLAUDIA, S_COUNT };
static const char *const SPR_NAMES[S_COUNT] = { "buggy", "hornet", "firenza", "racer_blue", "racer_purple", "tank", "april", "fireball",
    "cactus", "rock_small", "rock_big", "mesa", "floor", "dome", "shot", "eshot", "explosion", "flash", "mine", "gate", "smoke", "claudia" };
typedef struct { int x, y, w, h, frames; } Spr;

/* ---- entities ---- */
enum { K_NONE, K_RACER, K_TANK, K_PROP, K_SHOT, K_ESHOT, K_MINE, K_EXPL, K_APRIL, K_BOSS, K_GATE, K_SMOKE, K_DOME, K_FLASH };
typedef struct {
    int kind, spr, frame; float anim;
    float x, y, z, heading, speed, vx, vy;
    float hp, t, t2, scale, tilt;
    int state; bool player_owned, hornet, solid;
    /* racers: on rails along the track */
    float s, lat, lat_target, max_speed; int lap, id; float knock;
    uint8_t r, g, b;   /* colour mod */
} Ent;
#define MAX_ENT 200

enum { PH_INTRO, PH_COUNTDOWN, PH_RACE, PH_BREAKAWAY, PH_PURSUIT, PH_ARRIVE, PH_BOSS, PH_VICTORY, PH_DEAD, PH_GAMEOVER, PH_CLEARED };

#define TRACK_N 1024
#define N_RACERS 7

struct Mode7 {
    SDL_Renderer *ren; int sw, sh;
    SDL_Texture *atlas; Spr spr[S_COUNT]; bool ok;
    uint32_t tiles[T_COUNT][CELL * CELL];
    uint8_t cells[WN * WN];
    SDL_Texture *floor_tex; uint32_t *floor_px; int floor_h;
    Level horizon; bool horizon_ok;
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
    float remaining, hornet_eta, pursuit_spawn_t, april_t;
    /* boss */
    float dome_hp, boss_hp_max, arena_x, arena_y; int boss_i;
    float race_time; int kills;
    unsigned rng;
    float dead_t; bool april_in; int resume_phase;
    char msg[64]; float msg_t;
    int music_now;
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

static uint8_t cell_at(const Mode7 *m, float x, float y) { int cx = ((int)floorf(x / CELL)) & (WN - 1), cy = ((int)floorf(y / CELL)) & (WN - 1); return m->cells[cy * WN + cx]; }
static void cell_set(Mode7 *m, int cx, int cy, uint8_t t) { m->cells[(cy & (WN - 1)) * WN + (cx & (WN - 1))] = t; }
static bool is_road(uint8_t t) { return t == T_ASPHALT || t == T_ASPHALT_L || t == T_ASPHALT_R || t == T_CHECKER || t == T_ASPHALT_DASH || t == T_DIRT || t == T_PLAZA; }
static bool is_rumble(uint8_t t) { return t == T_RUMBLE_A || t == T_RUMBLE_B; }

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
    /* floor tiles into memory */
    Spr *fl = &m->spr[S_FLOOR];
    for (int t = 0; t < T_COUNT && t < fl->frames; t++)
        for (int yy = 0; yy < CELL; yy++)
            memcpy(m->tiles[t] + yy * CELL, px + (size_t)(fl->y + yy) * w + fl->x + t * fl->w, CELL * 4);
    /* Claudia's avatar for the dialog system */
    Spr *cl = &m->spr[S_CLAUDIA];
    uint32_t *av = malloc((size_t)cl->w * cl->h * 4);
    for (int yy = 0; yy < cl->h; yy++) memcpy(av + yy * cl->w, px + (size_t)(cl->y + yy) * w + cl->x, cl->w * 4);
    sprite_from_rgba(namehash("dialog_avatar_claudia"), av, cl->w, cl->h, 1);
    free(av); free(px);
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
    for (int i = 0; i < WN * WN; i++) { float r = frand(m); m->cells[i] = r < 0.015f ? T_SAND_DARK : r < 0.5f ? T_SAND : T_SAND2; }
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
    /* rasterize: distance from the centreline decides the tile */
    fill_sand(m);
    const float HALF = 104, KERB = 128;
    for (int i = 0; i < TRACK_N; i++) {
        int cx0 = (int)floorf((m->tx[i] - KERB) / CELL), cx1 = (int)floorf((m->tx[i] + KERB) / CELL);
        int cy0 = (int)floorf((m->ty[i] - KERB) / CELL), cy1 = (int)floorf((m->ty[i] + KERB) / CELL);
        for (int cy = cy0; cy <= cy1; cy++) for (int cx = cx0; cx <= cx1; cx++) {
            float wx = cx * CELL + CELL / 2, wy = cy * CELL + CELL / 2;
            float d = hypotf(wx - m->tx[i], wy - m->ty[i]);
            uint8_t cur = m->cells[(cy & (WN - 1)) * WN + (cx & (WN - 1))];
            if (d < HALF) { if (!is_road(cur)) cell_set(m, cx, cy, ((cx * 7 + cy * 13) % 11 == 0) ? T_ASPHALT_DASH : T_ASPHALT); }
            else if (d < KERB && !is_road(cur) && !is_rumble(cur)) cell_set(m, cx, cy, ((cx + cy) & 1) ? T_RUMBLE_A : T_RUMBLE_B);
        }
    }
    /* road dashes should only sit near the centre: second pass keeps them within 20 units of the line */
    for (int i = 0; i < TRACK_N; i += 2) {
        int cx0 = (int)floorf((m->tx[i] - HALF) / CELL), cx1 = (int)floorf((m->tx[i] + HALF) / CELL);
        int cy0 = (int)floorf((m->ty[i] - HALF) / CELL), cy1 = (int)floorf((m->ty[i] + HALF) / CELL);
        for (int cy = cy0; cy <= cy1; cy++) for (int cx = cx0; cx <= cx1; cx++) {
            float wx = cx * CELL + CELL / 2, wy = cy * CELL + CELL / 2;
            float d = hypotf(wx - m->tx[i], wy - m->ty[i]);
            uint8_t cur = m->cells[(cy & (WN - 1)) * WN + (cx & (WN - 1))];
            if (cur == T_ASPHALT_DASH && d > 24) cell_set(m, cx, cy, T_ASPHALT);
        }
    }
    /* start / finish line across the track at sample 0 */
    {
        float nx = -(m->ty[1] - m->ty[0]), ny = m->tx[1] - m->tx[0]; float l = hypotf(nx, ny); nx /= l; ny /= l;
        for (float k = -HALF; k <= HALF; k += 8) cell_set(m, (int)floorf((m->tx[0] + nx * k) / CELL), (int)floorf((m->ty[0] + ny * k) / CELL), T_CHECKER);
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
    if (g) { g->kind = K_GATE; g->spr = S_GATE; g->x = m->tx[0]; g->y = m->ty[0]; g->scale = 1.6f; }
}

static void build_desert(Mode7 *m, bool plaza)
{
    fill_sand(m);
    int road_cx = WN / 2;
    for (int cy = 0; cy < WN; cy++) for (int cx = road_cx - 3; cx <= road_cx + 3; cx++) cell_set(m, cx, cy, T_DIRT);
    if (plaza) {
        int pcx = WN / 2, pcy = (int)floorf(m->arena_y / CELL);
        for (int cy = pcy - 22; cy <= pcy + 22; cy++) for (int cx = pcx - 22; cx <= pcx + 22; cx++)
            if ((cx - pcx) * (cx - pcx) + (cy - pcy) * (cy - pcy) <= 22 * 22) cell_set(m, cx, cy, T_PLAZA);
    }
}

/* ---------------------------------------------------------------- stage setup */
static const char *const SCRIPT_INTRO =
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nNew Borderland... the All Galaxy Grand Prix. I haven't sat on a grid like this since Cavalry Command recruited me.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nEnjoy it, Fireball. It's Marco Firenza's last race - beat him and you're the youngest champion twice over.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nKeep your eyes open. That Black Hornets team came out of nowhere and nobody has seen their drivers' faces.\n<<>>\n"
    "<|PURPLE|>\n</dialog_avatar_claudia/>\nFireball! I'm Claudia - your biggest fan! Meet me behind the paddock after qualifying? Alone?\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\n...Sure. Hey, what's- OUTRIDERS! It's a trap!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nFireball, get DOWN! ...You owe me one, hotshot. Now get back in that car - the race is about to start.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nThose Hornets are Outriders in disguise, pardner. Whatever they came for, it isn't the trophy. Watch 'em.\n<<>>\n"
    "<|BLUE|>\nSTEER left/right - ACCELERATE jump button or up - FIRE shoot button - TURBO aim button - BRAKE down.\n<<>>\n";
static const char *const SCRIPT_BREAKAWAY =
    "<|RED|>\n</dialog_avatar_april2/>\nThe Black Hornets are leaving the course! They're heading straight for Dome City - the Cavalry Command Nerve Center!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nFirenza can keep his trophy. I'm going after them!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nApril will ride with you on Nova. Don't let them reach the dome, Fireball!\n<<>>\n";
static const char *const SCRIPT_ARRIVE =
    "<|PURPLE|>\n</dialog_avatar_outrider/>\nToo late, Star Sheriff! The Nerve Center falls today, and the Vapor Zone swallows the New Frontier!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nNot on my track, bug. Ramrod, if you can hear me - keep Dome City's shields up. I'll handle the Hornet.\n<<>>\n";
static const char *const SCRIPT_VICTORY =
    "<|GREEN|>\n</dialog_avatar_saber2/>\nThat's the last of the Black Hornets. Dome City is safe, Fireball.\n<<>>\n"
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
        { S_FIRENZA, 470, false, "FIRENZA" }, { S_HORNET, 445, true, "HORNET" }, { S_HORNET, 440, true, "HORNET" },
        { S_RBLUE, 420, false, "VEGA" }, { S_HORNET, 435, true, "HORNET" }, { S_RPURPLE, 410, false, "KELLY" }, { S_RBLUE, 400, false, "DUNN" },
    };
    for (int i = 0; i < N_RACERS; i++) {
        Ent *e = ent_new(m); if (!e) break;
        e->kind = K_RACER; e->spr = FIELD[i].spr; e->max_speed = FIELD[i].max; e->hornet = FIELD[i].hornet; e->id = i;
        e->s = m->track_len - 60.0f * (i + 1) - 30; e->lat = (i & 1) ? 48 : -48; e->lat_target = e->lat; e->hp = e->hornet ? 6 : 4; e->solid = true;
        track_point(m, e->s, e->lat, &e->x, &e->y, &e->heading); e->speed = 0;
    }
    /* player last on the grid (right column) */
    m->s = m->track_len - 60.0f * (N_RACERS + 1) - 30; m->lat = 48; m->lap = 0; m->progress = 0;
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
    m->horizon_ok = level_load(&m->horizon, 0x12DAD1A7);   /* level 1's sky + mountain layers */
    m->difficulty = difficulty; m->lives = lives;
    m->max_hp = difficulty == 0 ? 4 : difficulty == 1 ? 2 : 1; m->hp = m->max_hp;   /* hard: one hit = out */
    m->boost = 1.0f; m->music_now = -1;
    dialog_set_hero(HERO_FIREBALL);   /* the Grand Prix is Fireball's story whoever was picked */
    start_race(m);
    m->phase = PH_INTRO; m->phase_t = 0;
    play_music(m, 10, true);
    dialog_open_script(&m->dlg, SCRIPT_INTRO);
    if (SDL_getenv("SABER_M7PHASE")) {   /* debug: 1 race (no story), 2 pursuit, 3 boss */
        int ph = atoi(SDL_getenv("SABER_M7PHASE")); m->dlg.active = false;
        if (ph == 1) { m->phase = PH_COUNTDOWN; m->countdown = 3.99f; if (SDL_getenv("SABER_M7LAP")) { m->lap = 1; m->progress = m->track_len + m->s; } }
        else if (ph == 2) { m->phase = PH_PURSUIT; begin_pursuit(m); }
        else if (ph == 3) { m->phase = PH_BOSS; begin_boss(m); m->dome_hp = 100; if (SDL_getenv("SABER_M7BOSSHP")) m->ents[m->boss_i].hp = (float)atof(SDL_getenv("SABER_M7BOSSHP")); }
    }
    return m;
}

void mode7_destroy(Mode7 *m)
{
    if (!m) return;
    if (m->atlas) SDL_DestroyTexture(m->atlas);
    if (m->floor_tex) SDL_DestroyTexture(m->floor_tex);
    free(m->floor_px); free(m);
}

int mode7_result(const Mode7 *m) { return m->result; }

/* ---------------------------------------------------------------- combat helpers */
static void player_hurt(Mode7 *m, int dmg)
{
    if (m->hurt_t > 0 || m->phase == PH_DEAD || m->phase == PH_VICTORY || m->phase == PH_INTRO) return;
    m->hp -= dmg; m->hurt_t = 1.5f; m->shake = 0.4f; sfx_play(3, 0);
    if (m->hp <= 0) {
        m->hp = 0; spawn_expl(m, m->px, m->py, 1.6f); sfx_play(5, 0); sfx_play(6, 3);
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

static void damage_ent(Mode7 *m, Ent *e, float dmg)
{
    e->hp -= dmg; e->knock = 0.25f; sfx_play(14, 0);
    if (e->hp <= 0) {
        spawn_expl(m, e->x, e->y, e->kind == K_BOSS ? 2.5f : e->kind == K_TANK ? 1.8f : 1.2f);
        sfx_play(5, 0); sfx_play(6, 3); m->kills++;
        if (e->kind == K_RACER) { e->kind = K_NONE; set_msg(m, e->hornet ? "BLACK HORNET DESTROYED" : "RIVAL WRECKED", 2.0f); }
        else if (e->kind == K_BOSS) { e->kind = K_NONE; }
        else e->kind = K_NONE;
    }
}

/* ---------------------------------------------------------------- update: the player's car */
static void player_drive(Mode7 *m, const Input *in, float dt, bool free_drive)
{
    uint8_t tile = cell_at(m, m->px, m->py);
    bool road = is_road(tile), rumble = is_rumble(tile);
    float vmax = road ? 470.0f : rumble ? 380.0f : 250.0f;
    bool turbo = btn_down(in, BTN_AIM) && m->boost > 0.05f && m->spin_t <= 0;
    if (turbo) { vmax *= 1.35f; m->boost -= dt * 0.33f; if (m->boost < 0) m->boost = 0; }
    else m->boost = clampf(m->boost + dt * 0.08f, 0, 1);
    bool accel = btn_down(in, BTN_JUMP) || btn_down(in, BTN_UP);
    bool brake = btn_down(in, BTN_DOWN);
    if (m->spin_t > 0) { accel = false; m->spin_t -= dt; }
    if (accel) m->speed += (turbo ? 520.0f : 300.0f) * dt;
    else m->speed -= 120.0f * dt;
    if (brake) m->speed -= 500.0f * dt;
    if (m->speed > vmax) m->speed -= (m->speed - vmax) * (road ? 1.5f : 4.0f) * dt;
    if (m->speed < 0) m->speed = brake ? clampf(m->speed, -80, 0) : 0;
    float steer = (btn_down(in, BTN_LEFT) ? -1 : 0) + (btn_down(in, BTN_RIGHT) ? 1 : 0);
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
        int lap = (int)floorf(m->progress / m->track_len);
        if (lap > m->lap) { m->lap = lap; set_msg(m, lap == 1 ? "LAP 2" : "FINAL LAP", 2.0f); }
    }
}

/* ---------------------------------------------------------------- update: entities */
static void update_racers(Mode7 *m, float dt)
{
    /* rubber band around the player's progress so the pack stays on screen */
    for (int i = 0; i < MAX_ENT; i++) {
        Ent *e = &m->ents[i]; if (e->kind != K_RACER) continue;
        float prog = e->lap * m->track_len + e->s, gap = prog - m->progress;
        float target = e->max_speed;
        if (gap > 3000) target *= 0.55f; else if (gap > 1400) target *= 0.72f; else if (gap < -1200) target *= 1.18f;
        if (e->knock > 0) { e->knock -= dt; target *= 0.35f; }
        if (m->phase == PH_COUNTDOWN || m->phase == PH_INTRO) target = 0;
        e->speed += (target - e->speed) * clampf(dt * (e->speed < target ? 0.9f : 2.5f), 0, 1);
        /* racing line wobble */
        e->t -= dt;
        if (e->t <= 0) { e->t = 1.5f + frand(m) * 3; e->lat_target = (frand(m) - 0.5f) * 150; }
        e->lat += (e->lat_target - e->lat) * clampf(dt * 1.2f, 0, 1);
        float prev_s = e->s; e->s += e->speed * dt;
        if (e->s >= m->track_len) { e->s -= m->track_len; e->lap++; }
        (void)prev_s;
        track_point(m, e->s, e->lat, &e->x, &e->y, &e->heading);
        e->anim += e->speed * dt * 0.02f; e->frame = (int)e->anim & 1;
        /* Black Hornets fight: mines behind when ahead of the player, shots when behind */
        if (e->hornet && m->phase == PH_RACE && m->race_time > 8.0f && e->speed > 200) {
            e->t2 -= dt;
            if (e->t2 <= 0) {
                if (gap > 60 && gap < 900) { e->t2 = 2.5f + frand(m) * 2; Ent *mn = ent_new(m); if (mn) { mn->kind = K_MINE; mn->spr = S_MINE; mn->x = e->x; mn->y = e->y; mn->t = 12; } }
                else if (gap < -40 && gap > -700) { e->t2 = 1.6f + frand(m); float h = atan2f(dwrap(m->py, e->y), dwrap(m->px, e->x)); fire_shot(m, e->x, e->y, h, 700, false, 12); sfx_play(7, 0); }
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
                    if (o->kind != K_RACER && o->kind != K_TANK && o->kind != K_BOSS && o->kind != K_MINE) continue;
                    float r = o->kind == K_BOSS ? 90 : o->kind == K_TANK ? 48 : o->kind == K_MINE ? 18 : 34;
                    if (fabsf(dwrap(e->x, o->x)) < r && fabsf(dwrap(e->y, o->y)) < r) {
                        e->kind = K_NONE;
                        if (o->kind == K_MINE) { spawn_expl(m, o->x, o->y, 0.8f); o->kind = K_NONE; sfx_play(5, 0); }
                        else damage_ent(m, o, 1);
                        break;
                    }
                }
            } else if (fabsf(dwrap(e->x, m->px)) < 22 && fabsf(dwrap(e->y, m->py)) < 22) { e->kind = K_NONE; player_hurt(m, 1); }
            break;
        case K_FLASH: e->t -= dt; if (e->t <= 0) e->kind = K_NONE; break;
        case K_EXPL: e->t += dt; e->frame = (int)(e->t * 14); if (e->frame >= 6) e->kind = K_NONE; if (e->frame == 2 && e->t2 == 0) { e->t2 = 1; spawn_smoke(m, e->x, e->y, 20 * e->scale); } break;
        case K_SMOKE: e->t += dt; e->frame = (int)(e->t * 6); e->z += 30 * dt; if (e->frame >= 4) e->kind = K_NONE; break;
        case K_MINE:
            e->t -= dt; e->anim += dt; e->frame = ((int)(e->anim * 4)) & 1;
            if (e->t <= 0) { e->kind = K_NONE; break; }
            if (fabsf(dwrap(e->x, m->px)) < 26 && fabsf(dwrap(e->y, m->py)) < 26) { spawn_expl(m, e->x, e->y, 1.0f); e->kind = K_NONE; player_hurt(m, 1); m->speed *= 0.4f; m->spin_t = 0.6f; }
            break;
        case K_PROP: {
            float dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y); float r = e->spr == S_CACTUS ? 18 : e->spr == S_MESA ? 70 : e->spr == S_ROCK_B ? 40 : 24;
            float d = hypotf(dx, dy);
            if (d < r + 16) {
                float nx = dx / (d > 1 ? d : 1), ny = dy / (d > 1 ? d : 1);
                m->px = wrapf(e->x + nx * (r + 17)); m->py = wrapf(e->y + ny * (r + 17));
                if (m->speed > 150 && m->hurt_t <= 0) { player_hurt(m, 1); m->spin_t = 0.5f; }
                m->speed = -fabsf(m->speed) * 0.35f - 40; m->shake = 0.2f;   /* bounce off */
            }
            break; }
        case K_RACER: {
            float dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y); float d = hypotf(dx, dy);
            if (d < 44) bump_player(m, e, d, 44);
            break; }
        case K_TANK: {
            /* pursuit tanks: drive north ahead of the player, weave, stop to turn their gun on him */
            e->t -= dt;
            float dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y), d = hypotf(dx, dy);
            if (e->state == 0) {   /* fleeing north at 70 % of the player's pace, weaving */
                e->heading = -PI / 2 + sinf(e->anim) * 0.35f; e->anim += dt * 1.3f;
                e->speed = 250;
                if (e->t <= 0) { e->state = 1; e->t = 2.5f + frand(m) * 1.5f; e->speed = 0; }
            } else {               /* stopped, firing at the player */
                e->speed *= 0.9f; e->t2 -= dt;
                if (e->t2 <= 0 && d < 1400) { e->t2 = 0.9f; float h = atan2f(dy, dx); fire_shot(m, e->x, e->y, h + (frand(m) - 0.5f) * 0.15f, 620, false, 16); sfx_play(7, 0); }
                if (e->t <= 0) { e->state = 0; e->t = 3 + frand(m) * 3; }
            }
            e->x = wrapf(e->x + cosf(e->heading) * e->speed * dt); e->y = wrapf(e->y + sinf(e->heading) * e->speed * dt);
            if (e->knock > 0) e->knock -= dt;
            if (d < 60) { bump_player(m, e, d, 60); if (m->hurt_t <= 0) { player_hurt(m, 1); m->spin_t = 0.5f; } m->speed *= 0.3f; }
            /* fell too far behind the player (he passed it): drop it */
            if (dy < -1800 || dy > 5000) e->kind = K_NONE;
            break; }
        case K_APRIL: {
            /* April on Nova rides at the player's left, bobbing, and takes a shot at the nearest tank now and then */
            float tx = m->px + cosf(m->heading) * 130 - sinf(m->heading) * -100, ty = m->py + sinf(m->heading) * 130 + cosf(m->heading) * -100;
            e->x = wrapf(e->x + dwrap(tx, e->x) * clampf(dt * 2.5f, 0, 1)); e->y = wrapf(e->y + dwrap(ty, e->y) * clampf(dt * 2.5f, 0, 1));
            e->anim += dt * 7; e->frame = ((int)e->anim) & 1; e->z = 6 + 5 * sinf(e->anim * 0.9f);
            e->t -= dt;
            if (e->t <= 0) {
                e->t = 3.0f;
                Ent *best = NULL; float bd = 1e9f;
                for (int j = 0; j < MAX_ENT; j++) { Ent *o = &m->ents[j]; if (o->kind != K_TANK && o->kind != K_BOSS) continue; float d = hypotf(dwrap(o->x, e->x), dwrap(o->y, e->y)); if (d < bd && d < 1500) { bd = d; best = o; } }
                if (best) { float h = atan2f(dwrap(best->y, e->y), dwrap(best->x, e->x)); fire_shot(m, e->x, e->y, h, 1300, true, 30); sfx_play(1, 0); }
            }
            break; }
        case K_BOSS: {
            float ax = m->arena_x, ay = m->arena_y;
            e->t -= dt; if (e->knock > 0) e->knock -= dt;
            float dx = dwrap(m->px, e->x), dy = dwrap(m->py, e->y), d = hypotf(dx, dy);
            if (e->state == 0) {   /* orbit the plaza, shelling the dome and the player */
                e->anim += dt * 0.55f;
                float tx = ax + cosf(e->anim) * 430, ty = ay + sinf(e->anim) * 430;
                e->heading = atan2f(dwrap(ty, e->y), dwrap(tx, e->x)); e->speed = 240;
                e->t2 -= dt;
                if (e->t2 <= 0) {
                    e->t2 = 1.3f;
                    float h = atan2f(dy, dx); fire_shot(m, e->x, e->y, h + (frand(m) - 0.5f) * 0.2f, 680, false, 24); sfx_play(7, 0);
                    if (((int)(e->anim * 10)) % 4 == 0) { m->dome_hp -= 5; if (m->dome_hp < 0) m->dome_hp = 0; spawn_expl(m, ax + (frand(m) - 0.5f) * 200, ay - 600, 1.4f); set_msg(m, "DOME CITY UNDER FIRE", 1.0f); }
                }
                if (e->t <= 0) { e->state = frand(m) < 0.5f ? 1 : 2; e->t = e->state == 1 ? 2.2f : 3.0f; if (e->state == 1) { set_msg(m, "HORNET CHARGING", 1.0f); sfx_play(0x13, 0); } }
            } else if (e->state == 1) {   /* charge the player */
                e->heading += angdiff(atan2f(dy, dx), e->heading) * clampf(dt * 2.5f, 0, 1); e->speed = 620;
                if (e->t <= 0) { e->state = 0; e->t = 6 + frand(m) * 3; }
            } else {                      /* mine ring while orbiting */
                e->anim += dt * 0.8f;
                float tx = ax + cosf(e->anim) * 430, ty = ay + sinf(e->anim) * 430;
                e->heading = atan2f(dwrap(ty, e->y), dwrap(tx, e->x)); e->speed = 300;
                e->t2 -= dt; if (e->t2 <= 0) { e->t2 = 0.35f; Ent *mn = ent_new(m); if (mn) { mn->kind = K_MINE; mn->spr = S_MINE; mn->x = e->x; mn->y = e->y; mn->t = 20; } }
                if (e->t <= 0) { e->state = 0; e->t = 6 + frand(m) * 3; }
            }
            e->x = wrapf(e->x + cosf(e->heading) * e->speed * dt); e->y = wrapf(e->y + sinf(e->heading) * e->speed * dt);
            if (d < 95) { bump_player(m, e, d, 95); if (m->hurt_t <= 0) { player_hurt(m, 1); m->spin_t = 0.6f; } m->speed *= 0.2f; }
            if (e->hp < m->boss_hp_max * 0.4f && ((int)(m->phase_t * 6) & 1)) spawn_smoke(m, e->x, e->y, 60);
            break; }
        default: break;
        }
    }
}

/* ---------------------------------------------------------------- phases */
static void begin_pursuit(Mode7 *m)
{
    ents_clear(m, false);
    build_desert(m, false);
    m->px = WORLD * 0.5f; m->py = WORLD * 0.5f; m->heading = m->cam_heading = -PI / 2; m->speed = 200;
    m->remaining = 26000; m->hornet_eta = 75; m->pursuit_spawn_t = 1.5f; m->april_t = 4; m->april_in = false;
    for (int i = 0; i < 120; i++) {
        Ent *e = ent_new(m); if (!e) break;
        float x = frand(m) * WORLD, y = frand(m) * WORLD;
        if (fabsf(dwrap(x, WORLD * 0.5f)) < 150) { e->kind = K_NONE; continue; }
        float r = frand(m);
        e->kind = K_PROP; e->spr = r < 0.5f ? S_CACTUS : r < 0.85f ? S_ROCK_S : r < 0.96f ? S_ROCK_B : S_MESA; e->x = x; e->y = y;
        e->scale = e->spr == S_MESA ? 2.2f : e->spr == S_ROCK_B ? 1.5f : 1.2f; e->solid = true;
    }
    play_music(m, 14, true);
    set_msg(m, "PURSUIT - REACH DOME CITY", 3.0f);
}

static void begin_boss(Mode7 *m)
{
    ents_clear(m, false);
    m->arena_x = WORLD * 0.5f; m->arena_y = WORLD * 0.5f - 400;
    build_desert(m, true);
    m->px = WORLD * 0.5f; m->py = WORLD * 0.5f + 500; m->heading = m->cam_heading = -PI / 2; m->speed = 0;
    Ent *b = ent_new(m);
    b->kind = K_BOSS; b->spr = S_TANK; b->scale = 1.7f; b->hp = m->boss_hp_max = m->difficulty == 0 ? 40 : m->difficulty == 1 ? 55 : 70;
    b->x = m->arena_x + 430; b->y = m->arena_y; b->state = 0; b->t = 5; b->t2 = 1.5f; b->r = 255; b->g = 150; b->b = 150; b->solid = true;
    m->boss_i = (int)(b - m->ents);
    Ent *d = ent_new(m); d->kind = K_DOME; d->spr = S_DOME; d->x = m->arena_x; d->y = m->arena_y - 760; d->scale = 3.0f;
    Ent *a = ent_new(m); a->kind = K_APRIL; a->spr = S_APRIL; a->x = m->px - 150; a->y = m->py; a->t = 2; a->scale = 0.9f;
    play_music(m, 17, true);
}

static void begin_victory(Mode7 *m)
{
    m->phase = PH_VICTORY; m->phase_t = 0; m->speed = 0;
    for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE;
    music_play(6, false); m->music_now = 6;
    dialog_open_script(&m->dlg, SCRIPT_VICTORY);
}

void mode7_update(Mode7 *m, const Input *in, float dt)
{
    if (!m->ok) { m->result = 1; return; }
    if (m->result) return;
    if (btn_pressed(in, BTN_PAUSE) && (m->phase == PH_RACE || m->phase == PH_PURSUIT || m->phase == PH_BOSS)) {
        m->paused = !m->paused; sfx_play(10, 0); music_pause(m->paused);
    }
    if (m->paused) return;
    m->phase_t += dt;
    if (m->msg_t > 0) m->msg_t -= dt;
    if (m->hurt_t > 0) m->hurt_t -= dt;
    if (m->shake > 0) m->shake -= dt;
    Input idle = { 0 }; for (int b = 0; b < BTN_COUNT; b++) idle.state[b] = 1;

    switch (m->phase) {
    case PH_INTRO:
        if (m->dlg.active) dialog_update(&m->dlg, in, dt);
        else { m->phase = PH_COUNTDOWN; m->phase_t = 0; m->countdown = 3.99f; }
        break;
    case PH_COUNTDOWN: {
        int before = (int)m->countdown; m->countdown -= dt; int after = (int)m->countdown;
        if (after != before) sfx_play(0, 0);
        update_racers(m, dt);
        if (m->countdown <= 1.0f) { m->phase = PH_RACE; m->phase_t = 0; set_msg(m, "GO!", 1.0f); sfx_play(8, 0); }
        break; }
    case PH_RACE:
        m->race_time += dt;
        player_drive(m, in, dt, false);
        update_racers(m, dt); update_ents(m, dt);
        /* standings */
        { int ahead = 0; for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_RACER && m->ents[i].lap * m->track_len + m->ents[i].s > m->progress) ahead++; m->rank = ahead + 1; }
        if (m->lap >= 1 && m->phase_t > 5) {   /* the Hornets break away at the start of lap 2 */
            m->phase = PH_BREAKAWAY; m->phase_t = 0; m->speed *= 0.5f;
            dialog_open_script(&m->dlg, SCRIPT_BREAKAWAY);
            for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_RACER && m->ents[i].hornet) m->ents[i].lat_target = 400;   /* off the course */
        }
        break;
    case PH_BREAKAWAY:
        update_racers(m, dt); update_ents(m, dt);
        if (m->dlg.active) dialog_update(&m->dlg, in, dt);
        else { m->phase = PH_PURSUIT; m->phase_t = 0; begin_pursuit(m); }
        break;
    case PH_PURSUIT: {
        player_drive(m, in, dt, true);
        update_ents(m, dt);
        float north = -m->vy;   /* progress toward Dome City = northward speed */
        if (north > 0) m->remaining -= north * dt;
        m->hornet_eta -= dt;
        if (!m->april_in && (m->april_t -= dt) <= 0) {
            m->april_in = true; Ent *a = ent_new(m);
            if (a) { a->kind = K_APRIL; a->spr = S_APRIL; a->x = m->px - 150; a->y = m->py + 300; a->t = 2; a->scale = 0.9f; }
            set_msg(m, "APRIL JOINS ON NOVA", 2.0f);
        }
        m->pursuit_spawn_t -= dt;
        if (m->pursuit_spawn_t <= 0 && m->remaining > 2500) {
            m->pursuit_spawn_t = 2.2f + frand(m) * 1.5f;
            int n = 1 + (frand(m) < 0.4f ? 1 : 0) + (m->difficulty == 2 ? 1 : 0);
            for (int k = 0; k < n; k++) {
                Ent *e = ent_new(m); if (!e) break;
                e->kind = K_TANK; e->spr = S_TANK; e->hp = 4 + m->difficulty; e->scale = 1.0f; e->solid = true;
                e->x = wrapf(m->px + (frand(m) - 0.5f) * 420); e->y = wrapf(m->py - 750 - frand(m) * 400); e->state = 0; e->t = 1.0f + frand(m) * 2; e->heading = -PI / 2;
            }
        }
        /* keep the car roughly northbound: the road is the way, but the desert is open */
        if (m->remaining <= 0) {
            m->phase = PH_ARRIVE; m->phase_t = 0; begin_boss(m);
            if (m->hornet_eta < 0) { m->dome_hp = 55; set_msg(m, "THE HORNETS GOT THERE FIRST", 3.0f); } else m->dome_hp = 100;
            dialog_open_script(&m->dlg, SCRIPT_ARRIVE);
        }
        break; }
    case PH_ARRIVE:
        if (m->dlg.active) dialog_update(&m->dlg, in, dt);
        else { m->phase = PH_BOSS; m->phase_t = 0; set_msg(m, "DESTROY THE HORNET LEADER", 3.0f); }
        break;
    case PH_BOSS: {
        player_drive(m, in, dt, true);
        update_ents(m, dt);
        Ent *b = &m->ents[m->boss_i];
        if (b->kind != K_BOSS) { begin_victory(m); break; }
        if (SDL_getenv("SABER_TRACE") && ((int)(m->phase_t * 60) % 60) == 0) fprintf(stderr, "boss st=%d pos=%.0f,%.0f hp=%.0f | player %.0f,%.0f h=%.2f arena %.0f,%.0f\n", b->state, b->x, b->y, b->hp, m->px, m->py, m->heading, m->arena_x, m->arena_y);
        if (m->dome_hp <= 0) { set_msg(m, "THE NERVE CENTER IS LOST", 3.0f); m->hp = 0; spawn_expl(m, m->px, m->py, 1.6f); m->phase = PH_DEAD; m->phase_t = 0; m->lives = 0; }
        /* leash: stay near the plaza */
        { float dx = dwrap(m->px, m->arena_x), dy = dwrap(m->py, m->arena_y), d = hypotf(dx, dy);
          if (d > 1000) { m->px = wrapf(m->arena_x + dx / d * 1000); m->py = wrapf(m->arena_y + dy / d * 1000); m->speed *= 0.3f; m->shake = 0.2f; set_msg(m, "STAY WITH THE DOME", 1.0f); }
          /* the dome itself is solid */
          float ex = dwrap(m->px, m->arena_x), ey = dwrap(m->py, m->arena_y - 760), ed = hypotf(ex, ey);
          if (ed < 330) { m->px = wrapf(m->arena_x + ex / ed * 330); m->py = wrapf(m->arena_y - 760 + ey / ed * 330); m->speed = -fabsf(m->speed) * 0.3f; m->shake = 0.2f; } }
        break; }
    case PH_VICTORY:
        update_ents(m, dt);
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
                m->lives--; m->hp = m->max_hp; m->hurt_t = 2.0f; m->speed = 0; m->spin_t = 0; m->boost = 1;
                /* back onto the course / the road */
                if (m->resume_phase == PH_BOSS) { m->px = m->arena_x; m->py = m->arena_y + 500; m->heading = m->cam_heading = -PI / 2; m->phase = PH_BOSS; for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_MINE || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE; }
                else if (m->resume_phase == PH_PURSUIT) { m->px = WORLD * 0.5f; m->heading = m->cam_heading = -PI / 2; m->phase = PH_PURSUIT; for (int i = 0; i < MAX_ENT; i++) if (m->ents[i].kind == K_TANK || m->ents[i].kind == K_ESHOT) m->ents[i].kind = K_NONE; }
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
    (void)idle;
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
        float step = d / FOCAL;
        float fog = clampf((d - FOG0) / (FOG1 - FOG0), 0, 1);
        int fa = (int)(fog * 256);
        float wx = camx + fx * d - rx * step * (sw * 0.5f), wy = camy + fy * d - ry * step * (sw * 0.5f);
        uint32_t *out = m->floor_px + (size_t)row * sw;
        for (int x = 0; x < sw; x++) {
            int ix = (int)floorf(wx), iy = (int)floorf(wy);
            uint8_t t = m->cells[((iy >> 5) & (WN - 1)) * WN + ((ix >> 5) & (WN - 1))];
            uint32_t c = m->tiles[t][(iy & 31) * CELL + (ix & 31)];
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
    /* level-1 sky + three mountain layers, scrolled by the heading (a full turn = one strip period) */
    SDL_SetRenderDrawColor(m->ren, 78, 160, 214, 255);
    SDL_FRect sky = { 0, 0, (float)m->sw, (float)(HORIZON + 1) }; SDL_RenderFillRect(m->ren, &sky);
    if (!m->horizon_ok) return;
    float turn = m->cam_heading / TWO_PI;
    const struct { const char *name; float period, speed; float oy; } LY[4] = {
        { "SkyBG", 976, 976, 0 }, { "FarMountains", 1280, 1280, 44 }, { "Mountains", 1248, 1248 * 1.0f, 44 }, { "NearMountains", 1280, 1280, 44 } };
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < m->horizon.nlayers; i++) {
            if (strcmp(m->horizon.layers[i].name, LY[k].name)) continue;
            level_draw_layer_strip(&m->horizon, i, turn * LY[k].speed * (k == 0 ? 0.5f : 1.0f) + k * 300, LY[k].oy, (int)LY[k].period, m->sw, HORIZON + 1);
        }
    }
    /* the far Dome City on the horizon during the pursuit */
    if (m->phase == PH_PURSUIT) {
        float f = 1.0f - clampf(m->remaining / 26000.0f, 0, 1);
        float sc = 0.15f + f * f * 1.4f;
        float rel = angdiff(-PI / 2, m->cam_heading);
        float sx = m->sw * 0.5f - rel * (m->sw / 1.6f);
        draw_spr(m, S_DOME, 0, sx, HORIZON + 2, sc, 0, 255, 255, 255, 255);
    }
}

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
        if (e->kind == K_RACER) frame = ((int)e->anim) % (m->spr[e->spr].frames > 1 ? 2 : 1);
        float ang = 0;
        if (e->kind == K_RACER) ang = clampf(angdiff(e->heading, m->cam_heading) * 20, -12, 12);
        draw_spr(m, e->spr, frame, items[i].sx, items[i].sy, items[i].scale, ang, r, g, b, a);
        if (e->kind == K_BOSS) {   /* boss shadow ring so the charge reads */
            SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(m->ren, 0, 0, 0, 60);
            SDL_FRect sh = { items[i].sx - 40 * items[i].scale, items[i].sy - 4, 80 * items[i].scale, 6 }; SDL_RenderFillRect(m->ren, &sh);
        }
    }
}

static void render_player(Mode7 *m)
{
    if (m->phase == PH_DEAD) return;
    float sy = HORIZON + CAM_H * FOCAL / CAM_BACK - 6 - m->bounce;
    float sx = m->sw * 0.5f + m->tilt * 1.2f;
    if (m->shake > 0) { sx += (float)(rand() % 5 - 2); sy += (float)(rand() % 3 - 1); }
    int frame = ((int)m->anim_t) % 6;
    uint8_t r = 255, g = 255, b = 255;
    if (m->hurt_t > 0 && ((int)(m->hurt_t * 20) & 1)) { r = 255; g = 90; b = 90; }
    float ang = m->spin_t > 0 ? m->spin_t * 720 : m->tilt;
    if (m->phase == PH_VICTORY && m->phase_t > 1.0f) {
        /* Fireball hops out and cheers next to the car */
        draw_spr(m, S_BUGGY, 0, sx, sy, 1, 0, r, g, b, 255);
        int f = (int)((m->phase_t - 1.0f) * 8) % m->spr[S_FIREBALL].frames;
        draw_spr(m, S_FIREBALL, f, sx + 60, sy + 4, 1, 0, 255, 255, 255, 255);
        return;
    }
    draw_spr(m, S_BUGGY, frame, sx, sy, 1, ang, r, g, b, 255);
}

static void bar(SDL_Renderer *r, float x, float y, float w, float h, float f, uint8_t R, uint8_t G, uint8_t B)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 160); SDL_FRect bg = { x - 1, y - 1, w + 2, h + 2 }; SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, R, G, B, 255); SDL_FRect fg = { x, y, w * clampf(f, 0, 1), h }; SDL_RenderFillRect(r, &fg);
}

static void render_hud(Mode7 *m)
{
    hud_draw(m->ren, HERO_FIREBALL, m->difficulty == 2 ? 2 : m->difficulty, m->lives, m->hp, 0);
    Font *f = font_get(0x4058897F), *small = font_get(0x12072E60);
    if (!f || !small) return;
    char buf[64];
    /* turbo meter + speed */
    bar(m->ren, 8, m->sh - 14, 60, 5, m->boost, 255, 182, 0);
    font_draw(small, "TURBO", 8, (float)(m->sh - 26), 255, 182, 0);
    snprintf(buf, sizeof buf, "%3d", (int)(fabsf(m->speed) * 0.6f));
    font_draw(f, buf, (float)(m->sw - 8 - font_text_width(f, buf)), (float)(m->sh - 20), 255, 255, 255);
    font_draw(small, "KM/H", (float)(m->sw - 12 - font_text_width(small, "KM/H")), (float)(m->sh - 30), 200, 200, 200);
    if (m->phase == PH_RACE || m->phase == PH_COUNTDOWN || m->phase == PH_BREAKAWAY) {
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
    } else if (m->phase == PH_PURSUIT) {
        font_draw(small, "DOME CITY", (float)(m->sw / 2 - 30), 6, 255, 255, 255);
        bar(m->ren, (float)(m->sw / 2 - 60), 18, 120, 5, 1.0f - m->remaining / 26000.0f, 90, 200, 255);
        snprintf(buf, sizeof buf, "HORNETS ETA %02d", m->hornet_eta > 0 ? (int)m->hornet_eta : 0);
        font_draw(small, buf, (float)(m->sw - 8 - font_text_width(small, buf)), 8, m->hornet_eta < 15 ? 255 : 255, m->hornet_eta < 15 ? 80 : 255, m->hornet_eta < 15 ? 80 : 255);
    } else if (m->phase == PH_BOSS || m->phase == PH_ARRIVE) {
        Ent *b = &m->ents[m->boss_i];
        font_draw(small, "HORNET LEADER", (float)(m->sw / 2 - 40), 6, 255, 120, 120);
        bar(m->ren, (float)(m->sw / 2 - 70), 18, 140, 5, b->kind == K_BOSS ? b->hp / m->boss_hp_max : 0, 230, 50, 50);
        font_draw(small, "DOME CITY", (float)(m->sw - 8 - 60), 6, 120, 200, 255);
        bar(m->ren, (float)(m->sw - 8 - 100), 18, 100, 5, m->dome_hp / 100.0f, 90, 200, 255);
        if (b->kind == K_BOSS) {   /* where is it? a red arrow along the screen edge when the leader is off screen */
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
    if (m->paused) {
        SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(m->ren, 0, 0, 0, 64);
        SDL_FRect q = { 0, 0, (float)m->sw, (float)m->sh }; SDL_RenderFillRect(m->ren, &q);
        Sprite *ps = sprite_get(0xB2143E42);
        if (ps && ((SDL_GetTicks() / 16) & 0x7f) > 0x30) sprite_draw(ps, 0, (float)((m->sw - ps->w) / 2), (float)((m->sh - ps->h) / 2), false);
    }
}

void mode7_draw(Mode7 *m, bool scanlines)
{
    if (!m->ok) return;
    render_horizon(m);
    render_floor(m);
    render_sprites(m);
    render_player(m);
    render_hud(m);
    if (m->phase == PH_INTRO) {   /* letterbox for the story scene */
        SDL_SetRenderDrawColor(m->ren, 0, 0, 0, 255);
        SDL_FRect t = { 0, 0, (float)m->sw, 16 }, b = { 0, (float)(m->sh - 16), (float)m->sw, 16 }; SDL_RenderFillRect(m->ren, &t); SDL_RenderFillRect(m->ren, &b);
    }
    if (m->dlg.active) dialog_draw(&m->dlg, m->ren, m->sw, m->sh);
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
