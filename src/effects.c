#include "effects.h"
#include "pack.h"
#include <math.h>
#include <string.h>

Effect *effects_spawn(Effects *fx, uint32_t sprite_id, int layer, const AnimDef *a, float x, float y, float ox, float oy, float angle)
{
    for (int i = 0; i < MAX_EFFECTS; i++) {
        Effect *e = &fx->e[i];
        if (e->alive) continue;
        memset(e, 0, sizeof *e);
        const PackEntry *pe = packs_find(sprite_id);
        if (!pe) return NULL;
        if (pe->type == RES_CBLOCK) e->cb = cblock_get(sprite_id); else e->spr = sprite_get(sprite_id);
        e->alive = true; e->anim = *a; e->frame = a->first; e->layer = layer;
        e->x = x; e->y = y; e->ox = ox; e->oy = oy; e->angle = angle;
        return e;
    }
    return NULL;
}

void effects_update(Effects *fx, float dt)
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

void effects_draw(const Effects *fx, int layer, float cam_x, float cam_y)
{
    for (int i = 0; i < MAX_EFFECTS; i++) {
        const Effect *e = &fx->e[i];
        if (!e->alive || e->layer != layer) continue;
        float x = e->x + (e->follow_x ? *e->follow_x - e->fx0 : 0), y = e->y + (e->follow_y ? *e->follow_y - e->fy0 : 0);
        x = floorf(x - e->ox - cam_x); y = floorf(y - e->oy - cam_y);
        if (e->spr) {
            int f = e->frame < e->spr->frames ? e->frame : e->spr->frames - 1;
            if (e->angle != 0) {
                SDL_FRect src = { (float)(f * e->spr->w), 0, (float)e->spr->w, (float)e->spr->h };
                SDL_FRect dst = { x, y, (float)e->spr->w, (float)e->spr->h };
                SDL_RenderTextureRotated(SDL_GetRendererFromTexture(e->spr->tex), e->spr->tex, &src, &dst, -e->angle * 180.0 / 3.14159265, NULL, SDL_FLIP_NONE);
            } else sprite_draw(e->spr, f, x, y, e->flip);
        } else if (e->cb) {
            int ci = e->frame; if (ci >= 0 && ci < cblock_ncells(e->cb) && e->cb->cells[ci] != 0xFFFF) cblock_draw_tile(e->cb, e->cb->cells[ci], x, y, e->flip);
        }
    }
}
