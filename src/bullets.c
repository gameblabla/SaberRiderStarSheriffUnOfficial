#include "bullets.h"
#include "audio.h"
#include "physics.h"
#include <math.h>
#include <string.h>

static const uint32_t KIND_SPRITE[4] = { 0xF0FB3C78, 0x7027A26E, 0x33269F6B, 0xA2E02F5A };
static const float ANGLES[8] = { 3.1415927f, 2.3561945f, 1.5707964f, 0.7853982f, 0, 5.4977871f, 4.712389f, 3.9269908f };

void bullets_spawn(Bullets *bs, int kind, int layer, float x, float y, int dir, float speed)
{
    if (bs->n >= MAX_BULLETS) return;
    Bullet *b = &bs->b[bs->n++];
    memset(b, 0, sizeof *b);
    b->x = x; b->y = y; b->dir = (uint8_t)dir; b->speed = speed; b->kind = (uint8_t)kind; b->layer = layer;
    b->spr = sprite_get(KIND_SPRITE[kind & 3]);
    if (kind == BK_LASER && dir < 8) b->angle = ANGLES[dir];
}

static void remove_at(Bullets *bs, int i) { bs->b[i] = bs->b[--bs->n]; }

void bullets_update(Bullets *bs, const Level *L, Effects *fx, float dt, float cam_x, float cam_y, int sw, int sh)
{
    const float d7 = 0.7f * dt;
    for (int i = 0; i < bs->n; ) {
        Bullet *b = &bs->b[i];
        float x0 = b->x, y0 = b->y, nx, ny;
        float s = b->speed;
        bool ballistic = false;
        if (b->kind == BK_GRENADE) b->angle += 0.1f;
        switch (b->kind == BK_GRENADE ? b->dir + 8 : b->dir) {
        case 0: nx = x0 - s * dt; ny = y0; break;
        case 1: nx = x0 - s * d7; ny = y0 - s * d7; break;
        case 2: nx = x0; ny = y0 - s * dt; break;
        case 3: nx = x0 + s * d7; ny = y0 - s * d7; break;
        case 4: nx = x0 + s * dt; ny = y0; break;
        case 5: nx = x0 + s * d7; ny = y0 + s * d7; break;
        case 6: nx = x0; ny = y0 + s * dt; break;
        case 7: nx = x0 - s * d7; ny = y0 + s * d7; break;
        default: {   /* ballistic: dirs 8..15 give the horizontal component; vertical is a parabola */
            float tc = b->t < 1.0f ? b->t : 1.0f;
            float hx;
            switch (b->dir & 7) {
            case 0:  hx = -s * dt; break;
            case 1:  hx = -s * d7; break;
            case 3:  hx = s * d7; break;
            case 4:  hx = s * dt; break;
            case 5:  hx = s * d7; break;
            case 7:  hx = -s * d7; break;
            default: hx = 0; break;
            }
            nx = x0 + hx;
            ny = y0 + (tc * tc * 12.666667f - 0.015625f * s);
            ballistic = true;
        } }
        b->x = nx; b->y = ny; b->t += dt;
        /* tile hit test: bullets sample a small grid around the new position; grenades a wider one */
        bool hit = false;
        int cw = L->cellw, ch = L->cellh;
        float hxs = (x0 - nx) * 0.5f;
        int reach = b->kind == BK_GRENADE ? 2 : 1;
        for (int r = 0; r < 3 && !hit; r++)
            for (int c = 1; c < 4 && !hit; c++)
                for (int k = 0; k < reach && !hit; k++) {
                    float px = (c - 1) * hxs + x0;
                    float py = (k * 16.0f + (y0 - ny)) * (r * 0.5f) + y0;
                    int cx = (int)floorf(px / cw), cy = (int)floorf(py / ch);
                    uint8_t cell = level_cell(L, cx, cy);
                    if (cell == 15 || (cell & COLL_RAMP)) {   /* solid, or stage 4's ramp ground */
                        hit = true;
                        if (b->kind == BK_GRENADE) {
                            sfx_play(14, 0);
                            AnimDef a = { 0, 0, 12, 12, 0.0666667f, 0 };
                            effects_spawn(fx, 0x5B5EBBA3, b->layer, &a, px, py, 20, 32, 0);
                        }
                    }
                }
        (void)ballistic;
        float sx = nx - cam_x, sy = ny - cam_y;
        if (hit || sx < -64 || sx > sw + 64 || sy < -64 || sy > sh + 64) remove_at(bs, i);
        else i++;
    }
}

void bullets_draw(const Bullets *bs, int layer, float cam_x, float cam_y)
{
    for (int i = 0; i < bs->n; i++) {
        const Bullet *b = &bs->b[i];
        if (b->layer != layer || !b->spr) continue;
        float x = floorf(b->x - 4 - cam_x), y = floorf(b->y - 4 - cam_y);
        int f = 0;
        if (b->angle != 0) {
            SDL_FRect src = { 0, 0, (float)b->spr->w, (float)b->spr->h }, dst = { x, y, (float)b->spr->w, (float)b->spr->h };
            SDL_RenderTextureRotated(SDL_GetRendererFromTexture(b->spr->tex), b->spr->tex, &src, &dst, -b->angle * 180.0 / 3.14159265, NULL, SDL_FLIP_NONE);
        } else if (b->dark) sprite_draw_mod(b->spr, f, x, y, 200, 90, 255, 255);
        else sprite_draw(b->spr, f, x, y, false);
    }
}
