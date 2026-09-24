#include "effects.h"
#include "pack.h"
#include <string.h>

Effect *effects_spawn(Effects *fx, uint32_t sprite_id, int layer, const AnimDef *a, real x, real y, real ox, real oy, real angle)
{
    for (int i = 0; i < MAX_EFFECTS; i++) {
        Effect *e = &fx->e[i];
        if (e->alive) continue;
        memset(e, 0, sizeof *e);
        const PackEntry *pe = packs_peek(sprite_id);
        if (!pe) return NULL;
        if (pe->type == RES_CBLOCK) e->cb = cblock_get(sprite_id); else e->spr = sprite_get(sprite_id);
        e->alive = true; e->anim = *a; e->frame = a->first; e->layer = layer;
        e->x = x; e->y = y; e->ox = ox; e->oy = oy; e->angle = angle;
        return e;
    }
    return NULL;
}

void effects_update(Effects *fx, real dt)
{
    for (int i = 0; i < MAX_EFFECTS; i++) {
        Effect *e = &fx->e[i];
        if (!e->alive) continue;
        e->t += dt;
        bool rev = e->anim.first > e->anim.last;   /* first > last: played backwards (stage 4's warp-ins) */
        while (e->t >= e->anim.frame_time) {
            e->t -= e->anim.frame_time;
            if (rev ? e->frame > e->anim.last : e->frame < e->anim.last) e->frame += rev ? -1 : 1;
            else { e->alive = false; break; }     /* one-shot: dies after the last frame */
        }
    }
}

void effects_draw(const Effects *fx, int layer, real cam_x, real cam_y)
{
    for (int i = 0; i < MAX_EFFECTS; i++) {
        const Effect *e = &fx->e[i];
        if (!e->alive || e->layer != layer) continue;
        real x = e->x + (e->follow_x ? *e->follow_x - e->fx0 : 0), y = e->y + (e->follow_y ? *e->follow_y - e->fy0 : 0);
        x = r_floorr(x - e->ox - cam_x); y = r_floorr(y - e->oy - cam_y);
        if (e->spr) {
            int f = e->frame < e->spr->frames ? e->frame : e->spr->frames - 1;
            if (e->angle != 0) {
                RFRect src = { r_int(f * e->spr->w), 0, r_int(e->spr->w), r_int(e->spr->h) };
                RFRect dst = { x, y, r_int(e->spr->w), r_int(e->spr->h) };
                r_tex_rot(rtex_renderer(sprite_tex(e->spr)), sprite_tex(e->spr), &src, &dst, r_deg(-e->angle), NULL, R_FLIP_NONE);
            } else sprite_draw(e->spr, f, x, y, e->flip);
        } else if (e->cb) {
            int ci = e->frame; if (ci >= 0 && ci < cblock_ncells(e->cb) && e->cb->cells[ci] != 0xFFFF) cblock_draw_tile(e->cb, e->cb->cells[ci], x, y, e->flip);
        }
    }
}
