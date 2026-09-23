#include "power.h"
#include "audio.h"
#include "assets.h"
#include "font.h"
#include "gfx.h"
#include "heroes.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COOLDOWN      20.0f   /* platform stages: seconds after one power attack before the next */
#define BOMB_COOLDOWN 12.0f   /* the final phase */
#define CUT_DUR       1.9f    /* the drawn cut-in */
#define FLASH_DUR     0.7f    /* the flash the world comes back under */
#define SPEED_TIME    8.0f    /* April */
#define RAPID_TIME    10.0f   /* Colt */
#define SKIP_AFTER    0.4f    /* the power / pause button skips the rest of a cut-in after this long */

/* the select screen's portraits (lit) and the briefing's hero pieces: one diagonal slice of an anime cel per hero in
 * a 448x240 frame (cblock 2DEF1664), with the centre of its coloured part */
static const uint32_t PORTRAIT[4] = { 0x67C9A3D9, 0x74100546, 0x72A6B0FB, 0xEEE2331F };
static const int PIECE[4] = { 1, 0, 2, 3 };
static const float PIECE_C[4][2] = { { 123, 102 }, { 324, 142 }, { 272, 39 }, { 181, 196 } };   /* by frame */
static const int PIECE_X[4][2] = { { 60, 230 }, { 232, 386 }, { 150, 386 }, { 60, 313 } };           /* its coloured columns */
static const uint8_t BAND[4][3] = { { 14, 92, 40 }, { 150, 16, 18 }, { 160, 24, 96 }, { 26, 58, 150 } };     /* as the clips: Saber green, Fireball red */
static const uint8_t CORE[4][3] = { { 250, 236, 110 }, { 255, 214, 200 }, { 255, 196, 228 }, { 196, 226, 255 } };
static const uint8_t FLASH[4][3] = { { 206, 234, 255 }, { 255, 176, 90 }, { 255, 160, 214 }, { 170, 214, 255 } };
static const char *const NAME[4] = { "SABER RIDER", "FIREBALL", "APRIL", "COLT" };
static const char *const MOVE[4] = { "SABER SLASH!", "FIREBALL BLAST!", "OVERDRIVE!", "RAPID FIRE!" };
static const char *const CLIP[4] = { "power/saber", "power/fireball", NULL, NULL };

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float ease_out(float t) { t = clampf(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }
static int hero_of(const Power *pw) { return pw->hero & 3; }

void power_reset(Power *pw, int hero, bool bomb)
{
    power_close(pw);
    memset(pw, 0, sizeof *pw);
    pw->hero = hero & 3; pw->bomb = bomb; pw->items = POWER_ITEMS;
    if (plat_getenv("SABER_POWER")) pw->items = atoi(plat_getenv("SABER_POWER"));   /* debug: items at the start */
}

bool power_can_start(const Power *pw) { return pw->items > 0 && pw->cooldown <= 0 && pw->phase == PW_IDLE && pw->boost_t <= 0; }
bool power_in_cutin(const Power *pw) { return pw->phase == PW_CUTIN; }
bool power_speed(const Power *pw) { return !pw->bomb && hero_of(pw) == HERO_APRIL && pw->boost_t > 0; }
bool power_rapid(const Power *pw) { return !pw->bomb && hero_of(pw) == HERO_COLT && pw->boost_t > 0; }

void power_start(Power *pw, Ren *ren)
{
    if (!power_can_start(pw)) return;
    pw->items--; pw->phase = PW_CUTIN; pw->t = 0; pw->dur = CUT_DUR; pw->strike = false;
    music_set_duck(0.28f);
    sfx_play_file(asset_path("power/saber_intermission.wav"));
    const char *clip = pw->bomb ? NULL : CLIP[hero_of(pw)];
    if (clip) {
        char buf[64]; snprintf(buf, sizeof buf, "%s.m4v", clip);
        pw->video = video_open_file(ren, asset_path(buf), 24.0f);
        if (pw->video) { snprintf(buf, sizeof buf, "%s.wav", clip); voice_play_file(asset_path(buf)); pw->dur = 1e9f; }
    }
    if (!pw->video) sfx_play_file(asset_path("space/charge.wav"));
}

static void end_cutin(Power *pw)
{
    if (pw->video) { video_close(pw->video); pw->video = NULL; }
    pw->phase = PW_FLASH; pw->t = 0; pw->strike = true;
    pw->cooldown = pw->cooldown_max = pw->bomb ? BOMB_COOLDOWN : COOLDOWN;
    music_set_duck(1.0f);
    int h = hero_of(pw);
    if (!pw->bomb && h == HERO_APRIL) { pw->boost_t = pw->boost_max = SPEED_TIME; sfx_play_file(asset_path("sfx/turbo_start.wav")); }
    else if (!pw->bomb && h == HERO_COLT) { pw->boost_t = pw->boost_max = RAPID_TIME; sfx_play(1, 0); }
    else sfx_play(6, 0);
}

void power_update(Power *pw, const Input *in, float dt)
{
    if (pw->phase == PW_CUTIN) {
        float prev = pw->t; pw->t += dt;
        bool skip = pw->t > SKIP_AFTER && in && (btn_pressed(in, BTN_POWER) || btn_pressed(in, BTN_PAUSE));
        if (!pw->video && hero_of(pw) == HERO_APRIL && prev < 0.45f && pw->t >= 0.45f) voice_play_file(asset_path("voice/april_ok.wav"));
        if (pw->video) {
            if (!video_update(pw->video, dt) || skip) { if (skip) voice_stop(); end_cutin(pw); }
        } else if (pw->t >= pw->dur || skip) end_cutin(pw);
        return;
    }
    if (pw->phase == PW_FLASH && (pw->t += dt) >= FLASH_DUR) pw->phase = PW_IDLE;
    if (pw->cooldown > 0) pw->cooldown -= dt;
    if (pw->boost_t > 0) pw->boost_t -= dt;
}

bool power_take_strike(Power *pw) { bool s = pw->strike; pw->strike = false; return s; }

void power_close(Power *pw)
{
    if (pw->video) { video_close(pw->video); pw->video = NULL; voice_stop(); }
    if (pw->phase == PW_CUTIN) music_set_duck(1.0f);
    pw->phase = PW_IDLE;
}

/* ---- April's afterimages: a copy of the hero every 35 ms, each fading over a quarter second */
void power_trail_update(Power *pw, const Character *c, float dt)
{
    for (int i = 0; i < POWER_TRAIL; i++) if (pw->trail_life[i] > 0) pw->trail_life[i] -= dt;
    if (!power_speed(pw) || c->state == CS_DEAD) return;
    if ((pw->trail_t -= dt) > 0) return;
    pw->trail_t = 0.035f;
    pw->trail[pw->trail_i] = *c; pw->trail_life[pw->trail_i] = 0.25f;
    pw->trail_i = (pw->trail_i + 1) % POWER_TRAIL;
}

void power_draw_trail(const Power *pw, float cam_x, float cam_y)
{
    for (int k = 0; k < POWER_TRAIL; k++) {   /* oldest first */
        int i = (pw->trail_i + k) % POWER_TRAIL;
        const Character *c = &pw->trail[i];
        if (pw->trail_life[i] <= 0 || !c->cb || !cblock_tex(c->cb)) continue;
        RTex *t = cblock_tex(c->cb);
        rtex_set_color_mod(t, 255, 90 + 20 * k, 190); rtex_set_alpha_mod(t, (uint8_t)(170 * pw->trail_life[i] / 0.25f)); rtex_set_blend(t, R_BLEND_ADD);
        character_draw(c, cam_x, cam_y);
        rtex_set_color_mod(t, 255, 255, 255); rtex_set_alpha_mod(t, 255); rtex_set_blend(t, R_BLEND_BLEND);
    }
}

/* ---- drawing */
static void fill(Ren *r, float x, float y, float w, float h, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    r_set_draw_color(r, R, G, B, A); RFRect q = { x, y, w, h }; r_fill_rect(r, &q);
}

static void thick_line(Ren *r, float x0, float y0, float x1, float y1, int w)
{
    for (int k = -w; k <= w; k++) { r_line(r, x0 + k, y0, x1 + k, y1); r_line(r, x0, y0 + k, x1, y1 + k); }
}

/* the drawn cut-in, after the clips' opening: the screen darkens, a band in the hero's colour opens across the middle
 * with speed lines racing through it, the hero's face slides in from the right, the portrait from the left, the name
 * and the move type in, a glint, then everything burns out to white */
static void draw_cutin(const Power *pw, Ren *r, int sw, int sh)
{
    int h = hero_of(pw); float t = pw->t;
    r_set_draw_blend(r, R_BLEND_BLEND);
    fill(r, 0, 0, (float)sw, (float)sh, 0, 0, 0, (uint8_t)(170 * clampf(t / 0.15f, 0, 1)));
    float cy = sh * 0.5f, half = 64 * ease_out(t / 0.2f), top = cy - half, bot = cy + half;
    if (half < 1) return;
    /* the band: the hero's colour, brighter towards a white-hot core line */
    for (float y = top; y < bot; y += 1) {
        float k = 1 - fabsf(y - cy) / half, c = k * k * k;
        uint8_t R = (uint8_t)(BAND[h][0] + (CORE[h][0] - BAND[h][0]) * c), G = (uint8_t)(BAND[h][1] + (CORE[h][1] - BAND[h][1]) * c), B = (uint8_t)(BAND[h][2] + (CORE[h][2] - BAND[h][2]) * c);
        fill(r, 0, y, (float)sw, 1, R, G, B, 255);
    }
    fill(r, 0, top - 2, (float)sw, 2, 0, 0, 0, 255); fill(r, 0, bot, (float)sw, 2, 0, 0, 0, 255);
    RRect clip = { 0, (int)top, sw, (int)(bot - top) };
    r_set_clip(r, &clip);
    /* speed lines racing left */
    for (int i = 0; i < 44; i++) {
        unsigned s = (unsigned)i * 2654435761u;
        float y = top + 4 + (s % 1000) / 1000.0f * (bot - top - 8), len = 18 + (s >> 10) % 50, v = 700 + (s >> 16) % 600;
        float x = fmodf(((s >> 4) % 997) - t * v, (float)sw + 120); if (x < 0) x += sw + 120; x -= 60;
        fill(r, x, floorf(y), len, 1, 255, 255, 255, (uint8_t)(150 + (s >> 20) % 100));
        fill(r, x - 2, floorf(y), 2, 1, 255, 255, 255, 255);
    }
    /* the face from the briefing, sliding in from the right with a dark edge */
    CBlock *pc = cblock_get(0x2DEF1664);
    if (pc && t > 0.08f) {
        int f = PIECE[h];
        float tx = sw * 0.70f - PIECE_C[f][0], ty = cy - PIECE_C[f][1];
        tx = floorf(tx + (1 - ease_out((t - 0.08f) / 0.25f)) * sw * 0.8f + (t - 0.33f) * -12);   /* keeps drifting a little */
        RRect face = { (int)tx + PIECE_X[f][0], (int)top, PIECE_X[f][1] - PIECE_X[f][0], (int)(bot - top) };   /* not the black the briefing's collage filled around it */
        r_rect_intersect(&face, &clip, &face);
        r_set_clip(r, &face);
        cblock_draw_frame(pc, f, tx, floorf(ty), false);
    }
    r_set_clip(r, NULL);
    /* the portrait from the left, a drop shadow behind it, breaking out of the band */
    Sprite *po = sprite_get(PORTRAIT[h]);
    float px = 0;
    if (po && t > 0.3f) {
        px = floorf(-po->w + (sw * 0.06f + po->w) * ease_out((t - 0.3f) / 0.22f));
        float py = floorf(cy - po->h * 0.5f);
        sprite_draw_mod(po, 0, px + 4, py + 3, 0, 0, 0, 150);
        sprite_draw(po, 0, px, py, false);
        if (t > 0.72f && t < 1.05f) {   /* the glint */
            float g = (t - 0.72f) / 0.33f, gx = px + po->w * 0.7f, gy = py + po->h * 0.28f, a = sinf(g * 3.1415927f), l = 3 + 11 * a;
            r_set_draw_blend(r, R_BLEND_ADD); r_set_draw_color(r, 255, 255, 255, (uint8_t)(255 * a));
            r_line(r, gx - l, gy, gx + l, gy); r_line(r, gx, gy - l, gx, gy + l);
            r_line(r, gx - l * 0.4f, gy - l * 0.4f, gx + l * 0.4f, gy + l * 0.4f); r_line(r, gx - l * 0.4f, gy + l * 0.4f, gx + l * 0.4f, gy - l * 0.4f);
            r_set_draw_blend(r, R_BLEND_BLEND);
        }
    }
    /* the name and the move */
    Font *big = font_get(0x4058897F), *small = font_get(0x12072E60);
    float tx = (po ? sw * 0.06f + po->w : 90) + 10;
    if (big && t > 0.45f) {   /* twice the size on a dark caption strip (it runs over the face) */
        float sl = ease_out((t - 0.45f) / 0.15f), y = bot - 24 - big->h * 2.0f;
        r_set_draw_blend(r, R_BLEND_BLEND);
        fill(r, tx - 6 + (1 - sl) * sw, y - 3, (float)sw, bot - y + 3, 0, 0, 0, 140);
        font_draw_scaled(big, NAME[h], tx + 2 - (1 - sl) * 40, y + 2, 2.0f, 0, 0, 0);
        font_draw_scaled(big, NAME[h], tx - (1 - sl) * 40, y, 2.0f, 255, 255, 255);
    }
    if (small && t > 0.62f) {
        char move[40]; snprintf(move, sizeof move, "%s", pw->bomb ? "SHERIFF BOMB!" : MOVE[h]);
        int n = (int)((t - 0.62f) * 30); if (n > (int)strlen(move)) n = (int)strlen(move);
        font_draw_n(small, move, n, tx + 1, bot - 19 + 1, 0, 0, 0);
        font_draw_n(small, move, n, tx, bot - 19, CORE[h][0], CORE[h][1], CORE[h][2]);
    }
    /* burning out to white */
    float w = clampf((t - (pw->dur - 0.3f)) / 0.3f, 0, 1);
    if (w > 0) fill(r, 0, 0, (float)sw, (float)sh, 255, 255, 255, (uint8_t)(255 * w));
}

void power_draw(const Power *pw, Ren *r, int sw, int sh)
{
    int h = hero_of(pw);
    if (pw->phase == PW_CUTIN) {
        if (pw->video) {
            r_set_draw_blend(r, R_BLEND_BLEND);
            fill(r, 0, 0, (float)sw, (float)sh, 0, 0, 0, 255);
            video_draw(pw->video, r, sw, sh);
            float in = 1 - clampf(pw->t / 0.12f, 0, 1);   /* the clips open on white: cut from the game through it */
            if (in > 0) fill(r, 0, 0, (float)sw, (float)sh, 255, 255, 255, (uint8_t)(255 * in));
        } else draw_cutin(pw, r, sw, sh);
        return;
    }
    if (pw->phase != PW_FLASH) return;
    float k = 1 - pw->t / FLASH_DUR;
    r_set_draw_blend(r, R_BLEND_BLEND);
    /* white, going over to the hero's colour as it fades */
    float c = clampf(pw->t / 0.25f, 0, 1);
    fill(r, 0, 0, (float)sw, (float)sh, (uint8_t)(255 + (FLASH[h][0] - 255) * c), (uint8_t)(255 + (FLASH[h][1] - 255) * c), (uint8_t)(255 + (FLASH[h][2] - 255) * c), (uint8_t)(235 * k * k));
    if (!pw->bomb && h == HERO_SABER && pw->t < 0.45f) {   /* the saber's cuts across the screen */
        float a = 1 - pw->t / 0.45f;
        r_set_draw_blend(r, R_BLEND_ADD); r_set_draw_color(r, 200, 240, 255, (uint8_t)(255 * a));
        thick_line(r, -10, sh * 0.15f, (float)sw + 10, sh * 0.75f, 1);
        thick_line(r, sw * 0.2f, (float)sh + 10, sw * 0.85f, -10, 1);
        thick_line(r, -10, sh * 0.8f, (float)sw + 10, sh * 0.35f, 0);
        r_set_draw_blend(r, R_BLEND_BLEND);
    }
}

void power_draw_hud(const Power *pw, Ren *r, float x, float y, float w)
{
    r_set_draw_blend(r, R_BLEND_BLEND);
    if (pw->boost_t > 0 && pw->boost_max > 0) {   /* April's / Colt's power running out */
        int h = hero_of(pw);
        fill(r, x, y, w, 2, 0, 0, 0, 200);
        fill(r, x, y, floorf(w * pw->boost_t / pw->boost_max), 2, FLASH[h][0], FLASH[h][1], FLASH[h][2], 255);
    } else if (pw->cooldown > 0 && pw->items > 0 && pw->cooldown_max > 0) {   /* recharging */
        fill(r, x, y, w, 2, 0, 0, 0, 200);
        fill(r, x, y, floorf(w * (1 - pw->cooldown / pw->cooldown_max)), 2, 150, 150, 170, 255);
    }
}
