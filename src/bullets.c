#include "bullets.h"
#include "audio.h"
#include "physics.h"
#include <string.h>

static const uint32_t KIND_SPRITE[4] = { 0xF0FB3C78, 0x7027A26E, 0x33269F6B, 0xA2E02F5A };
static const real ANGLES[8] = { R(3.1415927f), R(2.3561945f), R(1.5707964f), R(0.7853982f), 0, R(5.4977871f), R(4.712389f), R(3.9269908f) };

/* a speed over one step, straight (SD) or along a diagonal (SD7: 0.7 of it; exactly 7/10 in fixed point) */
#ifdef REAL_FIXED
#define SD(s)  r_mul_dt(s, dt)
#define SD7(s) r_mul_dt((s) * 7 / 10, dt)
#else
#define SD(s)  ((s) * dt)
#define SD7(s) ((s) * d7)
#endif

void bullets_spawn(Bullets *bs, int kind, int layer, real x, real y, int dir, real speed)
{
    if (bs->n >= MAX_BULLETS) return;
    Bullet *b = &bs->b[bs->n++];
    memset(b, 0, sizeof *b);
    b->x = x; b->y = y; b->dir = (uint8_t)dir; b->speed = speed; b->kind = (uint8_t)kind; b->layer = layer;
    b->spr = sprite_get(KIND_SPRITE[kind & 3]);
    if (kind == BK_LASER && dir < 8) b->angle = ANGLES[dir];
}

static void remove_at(Bullets *bs, int i) { bs->b[i] = bs->b[--bs->n]; }

void bullets_update(Bullets *bs, const Level *L, Effects *fx, real dt, real cam_x, real cam_y, int sw, int sh)
{
#ifndef REAL_FIXED
    const real d7 = 0.7f * dt;
#endif
    for (int i = 0; i < bs->n; ) {
        Bullet *b = &bs->b[i];
        real x0 = b->x, y0 = b->y, nx, ny;
        real s = b->speed;
        bool ballistic = false;
        if (b->kind == BK_GRENADE) b->angle += R(0.1);
        switch (b->kind == BK_GRENADE ? b->dir + 8 : b->dir) {
        case 0: nx = x0 - SD(s); ny = y0; break;
        case 1: nx = x0 - SD7(s); ny = y0 - SD7(s); break;
        case 2: nx = x0; ny = y0 - SD(s); break;
        case 3: nx = x0 + SD7(s); ny = y0 - SD7(s); break;
        case 4: nx = x0 + SD(s); ny = y0; break;
        case 5: nx = x0 + SD7(s); ny = y0 + SD7(s); break;
        case 6: nx = x0; ny = y0 + SD(s); break;
        case 7: nx = x0 - SD7(s); ny = y0 + SD7(s); break;
        default: {   /* ballistic: dirs 8..15 give the horizontal component; vertical is a parabola */
            real tc = b->t < R(1) ? b->t : R(1);
            real hx;
            switch (b->dir & 7) {
            case 0:  hx = -SD(s); break;
            case 1:  hx = -SD7(s); break;
            case 3:  hx = SD7(s); break;
            case 4:  hx = SD(s); break;
            case 5:  hx = SD7(s); break;
            case 7:  hx = -SD7(s); break;
            default: hx = 0; break;
            }
            nx = x0 + hx;
            ny = y0 + (r_mul(r_mul(tc, tc), R(12.666667)) - r_mul(R(0.015625), s));
            ballistic = true;
        } }
        b->x = nx; b->y = ny; b->t += dt;
        /* tile hit test: bullets sample a small grid around the new position; grenades a wider one */
        bool hit = false;
        int cw = L->cellw, ch = L->cellh;
        real hxs = (x0 - nx) / 2;   /* * 0.5f */
        int reach = b->kind == BK_GRENADE ? 2 : 1;
        for (int r = 0; r < 3 && !hit; r++)
            for (int c = 1; c < 4 && !hit; c++)
                for (int k = 0; k < reach && !hit; k++) {
                    real px = (c - 1) * hxs + x0;
                    real py = r_mul(r_int(k * 16) + (y0 - ny), r_int(r) / 2) + y0;
                    int cx = r_floor(px / cw), cy = r_floor(py / ch);
                    uint8_t cell = level_cell(L, cx, cy);
                    if (cell == 15 || (cell & COLL_RAMP)) {   /* solid, or stage 4's ramp ground */
                        hit = true;
                        if (b->kind == BK_GRENADE) {
                            sfx_play(14, 0);
                            AnimDef a = { 0, 0, 12, 12, R(0.0666667), 0 };
                            effects_spawn(fx, 0x5B5EBBA3, b->layer, &a, px, py, R(20), R(32), 0);
                        }
                    }
                }
        (void)ballistic;
        real sx = nx - cam_x, sy = ny - cam_y;
        if (hit || sx < R(-64) || sx > r_int(sw + 64) || sy < R(-64) || sy > r_int(sh + 64)) remove_at(bs, i);
        else i++;
    }
}

void bullets_draw(const Bullets *bs, int layer, real cam_x, real cam_y)
{
    for (int i = 0; i < bs->n; i++) {
        const Bullet *b = &bs->b[i];
        if (b->layer != layer || !b->spr) continue;
        real x = r_floorr(b->x - R(4) - cam_x), y = r_floorr(b->y - R(4) - cam_y);
        int f = 0;
        if (b->angle != 0) {
            RFRect src = { 0, 0, r_int(b->spr->w), r_int(b->spr->h) }, dst = { x, y, r_int(b->spr->w), r_int(b->spr->h) };
            r_tex_rot(rtex_renderer(sprite_tex(b->spr)), sprite_tex(b->spr), &src, &dst, r_deg(-b->angle), NULL, R_FLIP_NONE);
        } else if (b->dark) sprite_draw_mod(b->spr, f, x, y, 200, 90, 255, 255);
        else sprite_draw(b->spr, f, x, y, false);
    }
}
