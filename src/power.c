#include "power.h"
#include "audio.h"
#include "assets.h"
#include "font.h"
#include "gfx.h"
#include "heroes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COOLDOWN      R(20.0f)   /* platform stages: seconds after one power attack before the next */
#define BOMB_COOLDOWN R(12.0f)   /* the final phase */
#define CUT_DUR       R(1.9f)    /* the drawn cut-in */
#define FLASH_DUR     R(0.7f)    /* the flash the world comes back under */
#define SPEED_TIME    R(8.0f)    /* April */
#define RAPID_TIME    R(10.0f)   /* Colt */
#define SKIP_AFTER    R(0.4f)    /* the power / pause button skips the rest of a cut-in after this long */

/* the select screen's portraits (lit) and the briefing's hero pieces: one diagonal slice of an anime cel per hero in
 * a 448x240 frame (cblock 2DEF1664), with the centre of its coloured part */
static const uint32_t PORTRAIT[4] = { 0x67C9A3D9, 0x74100546, 0x72A6B0FB, 0xEEE2331F };
static const int PIECE[4] = { 1, 0, 2, 3 };
static const real PIECE_C[4][2] = { { R(123), R(102) }, { R(324), R(142) }, { R(272), R(39) }, { R(181), R(196) } };   /* by frame */
static const int PIECE_X[4][2] = { { 60, 230 }, { 232, 386 }, { 150, 386 }, { 60, 313 } };           /* its coloured columns */
static const uint8_t BAND[4][3] = { { 14, 92, 40 }, { 150, 16, 18 }, { 160, 24, 96 }, { 26, 58, 150 } };     /* as the clips: Saber green, Fireball red */
static const uint8_t CORE[4][3] = { { 250, 236, 110 }, { 255, 214, 200 }, { 255, 196, 228 }, { 196, 226, 255 } };
static const uint8_t FLASH[4][3] = { { 206, 234, 255 }, { 255, 176, 90 }, { 255, 160, 214 }, { 170, 214, 255 } };
static const char *const NAME[4] = { "SABER RIDER", "FIREBALL", "APRIL", "COLT" };
static const char *const MOVE[4] = { "SABER SLASH!", "FIREBALL BLAST!", "OVERDRIVE!", "RAPID FIRE!" };
static const char *const CLIP[4] = { "power/saber", "power/fireball", NULL, NULL };

static real clampf(real v, real lo, real hi) { return v < lo ? lo : v > hi ? hi : v; }
static real ease_out(real t) { t = clampf(t, 0, R(1)); return R(1) - r_mul(r_mul(R(1) - t, R(1) - t), R(1) - t); }
static int hero_of(const Power *pw) { return pw->hero & 3; }

static void preload_clip(const Power *pw)
{
    const char *clip = pw->bomb ? NULL : CLIP[hero_of(pw)];
    if (!clip) { video_preload_file(NULL, 0); return; }
    char buf[64]; snprintf(buf, sizeof buf, "%s.m4v", clip);
    video_preload_file(asset_path(buf), R(24.0f));
}

void power_reset(Power *pw, int hero, bool bomb)
{
    power_close(pw);
    memset(pw, 0, sizeof *pw);
    pw->hero = hero & 3; pw->bomb = bomb; pw->items = POWER_ITEMS;
    if (plat_getenv("SABER_POWER")) pw->items = atoi(plat_getenv("SABER_POWER"));   /* debug: items at the start */
    preload_clip(pw);
    static const char *const SFX[] = { "power/saber_intermission.wav", "space/charge.wav", "sfx/turbo_start.wav", "voice/april_ok.wav" };
    for (size_t i = 0; i < sizeof SFX / sizeof *SFX; i++) sfx_preload_file(asset_path(SFX[i]));   /* loaded before they play */
}

bool power_can_start(const Power *pw) { return pw->items > 0 && pw->cooldown <= 0 && pw->phase == PW_IDLE && pw->boost_t <= 0; }
bool power_in_cutin(const Power *pw) { return pw->phase == PW_CUTIN; }
bool power_speed(const Power *pw) { return !pw->bomb && hero_of(pw) == HERO_APRIL && pw->boost_t > 0; }
bool power_rapid(const Power *pw) { return !pw->bomb && hero_of(pw) == HERO_COLT && pw->boost_t > 0; }

void power_start(Power *pw, Ren *ren)
{
    if (!power_can_start(pw)) return;
    pw->items--; pw->phase = PW_CUTIN; pw->t = 0; pw->dur = CUT_DUR; pw->strike = false;
    music_set_duck(R(0.28f));
    sfx_play_file(asset_path("power/saber_intermission.wav"));
    const char *clip = pw->bomb ? NULL : CLIP[hero_of(pw)];
    if (clip) {
        char buf[64]; snprintf(buf, sizeof buf, "%s.m4v", clip);
        pw->video = video_open_file(ren, asset_path(buf), R(24.0f));
        if (pw->video) {
#if !defined(PLAT_SATURN)
            /* PC/Dreamcast clips use the sidecar WAV.  Saturn's generated CPK
             * already multiplexes that WAV as ADX, so playing it here too would
             * double the voice over the movie audio. */
            snprintf(buf, sizeof buf, "%s.wav", clip);
            voice_play_file(asset_path(buf));
#endif
            pw->dur = R_MAX;
        }
    }
    if (!pw->video) sfx_play_file(asset_path("space/charge.wav"));
}

static void end_cutin(Power *pw)
{
    if (pw->video) { video_close(pw->video); pw->video = NULL; }
    pw->phase = PW_FLASH; pw->t = 0; pw->strike = true;
    pw->preload_pending = pw->items > 0 && !pw->bomb && CLIP[hero_of(pw)] != NULL;
    pw->cooldown = pw->cooldown_max = pw->bomb ? BOMB_COOLDOWN : COOLDOWN;
    music_set_duck(R(1.0f));
    int h = hero_of(pw);
    if (!pw->bomb && h == HERO_APRIL) { pw->boost_t = pw->boost_max = SPEED_TIME; sfx_play_file(asset_path("sfx/turbo_start.wav")); }
    else if (!pw->bomb && h == HERO_COLT) { pw->boost_t = pw->boost_max = RAPID_TIME; sfx_play(1, 0); }
    else sfx_play(6, 0);
}

void power_update(Power *pw, const Input *in, real dt)
{
    if (pw->phase == PW_CUTIN) {
        real prev = pw->t; pw->t += dt;
        bool skip = pw->t > SKIP_AFTER && in && (btn_pressed(in, BTN_POWER) || btn_pressed(in, BTN_PAUSE));
        if (!pw->video && hero_of(pw) == HERO_APRIL && prev < R(0.45f) && pw->t >= R(0.45f)) voice_play_file(asset_path("voice/april_ok.wav"));
        if (pw->video) {
            if (!video_update(pw->video, dt) || skip) { if (skip) voice_stop(); end_cutin(pw); }
        } else if (pw->t >= pw->dur || skip) end_cutin(pw);
        return;
    }
    if (pw->phase == PW_FLASH) {
        /* One flash frame is already on screen before this runs.  Warm the
         * remaining clip here so a second power press later has the same
         * low latency without freezing live gameplay on a CD seek. */
        if (pw->preload_pending) { preload_clip(pw); pw->preload_pending = false; }
        if ((pw->t += dt) >= FLASH_DUR) pw->phase = PW_IDLE;
    }
    if (pw->cooldown > 0) pw->cooldown -= dt;
    if (pw->boost_t > 0) pw->boost_t -= dt;
}

bool power_take_strike(Power *pw) { bool s = pw->strike; pw->strike = false; return s; }

void power_close(Power *pw)
{
    if (pw->video) { video_close(pw->video); pw->video = NULL; voice_stop(); }
    video_preload_file(NULL, 0);
    pw->preload_pending = false;
    if (pw->phase == PW_CUTIN) music_set_duck(R(1.0f));
    pw->phase = PW_IDLE;
}

/* ---- April's afterimages: a copy of the hero every 35 ms, each fading over a quarter second */
void power_trail_update(Power *pw, const Character *c, real dt)
{
    for (int i = 0; i < POWER_TRAIL; i++) if (pw->trail_life[i] > 0) pw->trail_life[i] -= dt;
    if (!power_speed(pw) || c->state == CS_DEAD) return;
    if ((pw->trail_t -= dt) > 0) return;
    pw->trail_t = R(0.035f);
    pw->trail[pw->trail_i] = *c; pw->trail_life[pw->trail_i] = R(0.25f);
    pw->trail_i = (pw->trail_i + 1) % POWER_TRAIL;
}

void power_draw_trail(const Power *pw, real cam_x, real cam_y)
{
    for (int k = 0; k < POWER_TRAIL; k++) {   /* oldest first */
        int i = (pw->trail_i + k) % POWER_TRAIL;
        const Character *c = &pw->trail[i];
        if (pw->trail_life[i] <= 0 || !c->cb || !cblock_tex(c->cb)) continue;
        RTex *t = cblock_tex(c->cb);
        rtex_set_color_mod(t, 255, 90 + 20 * k, 190); rtex_set_alpha_mod(t, (uint8_t)r_trunc(r_div(170 * pw->trail_life[i], R(0.25f)))); rtex_set_blend(t, R_BLEND_ADD);
        character_draw(c, cam_x, cam_y);
        rtex_set_color_mod(t, 255, 255, 255); rtex_set_alpha_mod(t, 255); rtex_set_blend(t, R_BLEND_BLEND);
    }
}

/* ---- drawing */
static void fill(Ren *r, real x, real y, real w, real h, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca)
{
    r_set_draw_color(r, cr, cg, cb, ca); RFRect q = { x, y, w, h }; r_fill_rect(r, &q);
}

static void thick_line(Ren *r, real x0, real y0, real x1, real y1, int w)
{
    for (int k = -w; k <= w; k++) { r_line(r, x0 + r_int(k), y0, x1 + r_int(k), y1); r_line(r, x0, y0 + r_int(k), x1, y1 + r_int(k)); }
}

/* the drawn cut-in, after the clips' opening: the screen darkens, a band in the hero's colour opens across the middle
 * with speed lines racing through it, the hero's face slides in from the right, the portrait from the left, the name
 * and the move type in, a glint, then everything burns out to white */
static void draw_cutin(const Power *pw, Ren *r, int sw, int sh)
{
    int h = hero_of(pw); real t = pw->t;
    real fsw = r_int(sw), fsh = r_int(sh);
    r_set_draw_blend(r, R_BLEND_BLEND);
    fill(r, 0, 0, fsw, fsh, 0, 0, 0, (uint8_t)r_trunc(170 * clampf(r_div(t, R(0.15f)), 0, R(1))));
    real cy = fsh / 2, half = 64 * ease_out(r_div(t, R(0.2f))), top = cy - half, bot = cy + half;   /* sh * 0.5f */
    if (half < R(1)) return;
    /* the band: the hero's colour, brighter towards a white-hot core line */
    for (real y = top; y < bot; y += R(1)) {
        real k = R(1) - r_div(r_abs(y - cy), half), c = r_mul(r_mul(k, k), k);
        uint8_t cr = (uint8_t)r_trunc(r_int(BAND[h][0]) + (CORE[h][0] - BAND[h][0]) * c), cg = (uint8_t)r_trunc(r_int(BAND[h][1]) + (CORE[h][1] - BAND[h][1]) * c),
                cb = (uint8_t)r_trunc(r_int(BAND[h][2]) + (CORE[h][2] - BAND[h][2]) * c);
        fill(r, 0, y, fsw, R(1), cr, cg, cb, 255);
    }
    fill(r, 0, top - R(2), fsw, R(2), 0, 0, 0, 255); fill(r, 0, bot, fsw, R(2), 0, 0, 0, 255);
    RRect clip = { 0, r_trunc(top), sw, r_trunc(bot - top) };
    r_set_clip(r, &clip);
    /* speed lines racing left */
    for (int i = 0; i < 44; i++) {
        unsigned s = (unsigned)i * 2654435761u;
        real y = top + R(4) + r_mul(r_int((int)(s % 1000)) / 1000, bot - top - R(8)), len = r_int((int)(18 + (s >> 10) % 50)), v = r_int((int)(700 + (s >> 16) % 600));
        real x = r_fmod(r_int((int)((s >> 4) % 997)) - r_mul(t, v), fsw + R(120)); if (x < 0) x += r_int(sw + 120); x -= R(60);
        fill(r, x, r_floorr(y), len, R(1), 255, 255, 255, (uint8_t)(150 + (s >> 20) % 100));
        fill(r, x - R(2), r_floorr(y), R(2), R(1), 255, 255, 255, 255);
    }
    /* the face from the briefing, sliding in from the right with a dark edge */
    CBlock *pc = cblock_get(0x2DEF1664);
    if (pc && t > R(0.08f)) {
        int f = PIECE[h];
        real tx = r_mul(fsw, R(0.70f)) - PIECE_C[f][0], ty = cy - PIECE_C[f][1];
        tx = r_floorr(tx + r_mul(r_mul(R(1) - ease_out(r_div(t - R(0.08f), R(0.25f))), fsw), R(0.8f)) + (t - R(0.33f)) * -12);   /* keeps drifting a little */
        RRect face = { r_trunc(tx) + PIECE_X[f][0], r_trunc(top), PIECE_X[f][1] - PIECE_X[f][0], r_trunc(bot - top) };   /* not the black the briefing's collage filled around it */
        r_rect_intersect(&face, &clip, &face);
        r_set_clip(r, &face);
        cblock_draw_frame(pc, f, tx, r_floorr(ty), false);
    }
    r_set_clip(r, NULL);
    /* the portrait from the left, a drop shadow behind it, breaking out of the band */
    Sprite *po = sprite_get(PORTRAIT[h]);
    real px = 0;
    if (po && t > R(0.3f)) {
        px = r_floorr(r_int(-po->w) + r_mul(r_mul(fsw, R(0.06f)) + r_int(po->w), ease_out(r_div(t - R(0.3f), R(0.22f)))));
        real py = r_floorr(cy - r_int(po->h) / 2);   /* po->h * 0.5f */
        sprite_draw_mod(po, 0, px + R(4), py + R(3), 0, 0, 0, 150);
        sprite_draw(po, 0, px, py, false);
        if (t > R(0.72f) && t < R(1.05f)) {   /* the glint */
            real g = r_div(t - R(0.72f), R(0.33f)), gx = px + r_mul(r_int(po->w), R(0.7f)), gy = py + r_mul(r_int(po->h), R(0.28f)), a = r_sin(r_mul(g, R(3.1415927f))), l = R(3) + 11 * a;
            real l4 = r_mul(l, R(0.4f));
            r_set_draw_blend(r, R_BLEND_ADD); r_set_draw_color(r, 255, 255, 255, (uint8_t)r_trunc(255 * a));
            r_line(r, gx - l, gy, gx + l, gy); r_line(r, gx, gy - l, gx, gy + l);
            r_line(r, gx - l4, gy - l4, gx + l4, gy + l4); r_line(r, gx - l4, gy + l4, gx + l4, gy - l4);
            r_set_draw_blend(r, R_BLEND_BLEND);
        }
    }
    /* the name and the move */
    Font *big = font_get(0x4058897F), *small = font_get(0x12072E60);
    real tx = (po ? r_mul(fsw, R(0.06f)) + r_int(po->w) : R(90)) + R(10);
    if (big && t > R(0.45f)) {   /* twice the size on a dark caption strip (it runs over the face) */
        real sl = ease_out(r_div(t - R(0.45f), R(0.15f))), y = bot - R(24) - r_mul(r_int(big->h), R(2.0f));
        r_set_draw_blend(r, R_BLEND_BLEND);
        fill(r, tx - R(6) + (R(1) - sl) * sw, y - R(3), fsw, bot - y + R(3), 0, 0, 0, 140);
        font_draw_scaled(big, NAME[h], tx + R(2) - (R(1) - sl) * 40, y + R(2), R(2.0f), 0, 0, 0);
        font_draw_scaled(big, NAME[h], tx - (R(1) - sl) * 40, y, R(2.0f), 255, 255, 255);
    }
    if (small && t > R(0.62f)) {
        char move[40]; snprintf(move, sizeof move, "%s", pw->bomb ? "SHERIFF BOMB!" : MOVE[h]);
        int n = r_trunc((t - R(0.62f)) * 30); if (n > (int)strlen(move)) n = (int)strlen(move);
        font_draw_n(small, move, n, tx + R(1), bot - R(19) + R(1), 0, 0, 0);
        font_draw_n(small, move, n, tx, bot - R(19), CORE[h][0], CORE[h][1], CORE[h][2]);
    }
    /* burning out to white */
    real w = clampf(r_div(t - (pw->dur - R(0.3f)), R(0.3f)), 0, R(1));
    if (w > 0) fill(r, 0, 0, fsw, fsh, 255, 255, 255, (uint8_t)r_trunc(255 * w));
}

void power_draw(const Power *pw, Ren *r, int sw, int sh)
{
    int h = hero_of(pw);
    if (pw->phase == PW_CUTIN) {
        if (pw->video) {
            r_set_draw_blend(r, R_BLEND_BLEND);
            fill(r, 0, 0, r_int(sw), r_int(sh), 0, 0, 0, 255);
            video_draw(pw->video, r, sw, sh);
            real in = R(1) - clampf(r_div(pw->t, R(0.12f)), 0, R(1));   /* the clips open on white: cut from the game through it */
            if (in > 0) fill(r, 0, 0, r_int(sw), r_int(sh), 255, 255, 255, (uint8_t)r_trunc(255 * in));
        } else draw_cutin(pw, r, sw, sh);
        return;
    }
    if (pw->phase != PW_FLASH) return;
    real k = R(1) - r_div(pw->t, FLASH_DUR);
    real fsw = r_int(sw), fsh = r_int(sh);
    r_set_draw_blend(r, R_BLEND_BLEND);
    /* white, going over to the hero's colour as it fades */
    real c = clampf(r_div(pw->t, R(0.25f)), 0, R(1));
    fill(r, 0, 0, fsw, fsh, (uint8_t)r_trunc(R(255) + (FLASH[h][0] - 255) * c), (uint8_t)r_trunc(R(255) + (FLASH[h][1] - 255) * c),
         (uint8_t)r_trunc(R(255) + (FLASH[h][2] - 255) * c), (uint8_t)r_trunc(r_mul(235 * k, k)));
    if (!pw->bomb && h == HERO_SABER && pw->t < R(0.45f)) {   /* the saber's cuts across the screen */
        real a = R(1) - r_div(pw->t, R(0.45f));
        r_set_draw_blend(r, R_BLEND_ADD); r_set_draw_color(r, 200, 240, 255, (uint8_t)r_trunc(255 * a));
        thick_line(r, R(-10), r_mul(fsh, R(0.15f)), fsw + R(10), r_mul(fsh, R(0.75f)), 1);
        thick_line(r, r_mul(fsw, R(0.2f)), fsh + R(10), r_mul(fsw, R(0.85f)), R(-10), 1);
        thick_line(r, R(-10), r_mul(fsh, R(0.8f)), fsw + R(10), r_mul(fsh, R(0.35f)), 0);
        r_set_draw_blend(r, R_BLEND_BLEND);
    }
}

void power_draw_hud(const Power *pw, Ren *r, real x, real y, real w)
{
    r_set_draw_blend(r, R_BLEND_BLEND);
    if (pw->boost_t > 0 && pw->boost_max > 0) {   /* April's / Colt's power running out */
        int h = hero_of(pw);
        fill(r, x, y, w, R(2), 0, 0, 0, 200);
        fill(r, x, y, r_floorr(r_muldiv(w, pw->boost_t, pw->boost_max)), R(2), FLASH[h][0], FLASH[h][1], FLASH[h][2], 255);
    } else if (pw->cooldown > 0 && pw->items > 0 && pw->cooldown_max > 0) {   /* recharging */
        fill(r, x, y, w, R(2), 0, 0, 0, 200);
        fill(r, x, y, r_floorr(r_mul(w, R(1) - r_div(pw->cooldown, pw->cooldown_max))), R(2), 150, 150, 170, 255);
    }
}
