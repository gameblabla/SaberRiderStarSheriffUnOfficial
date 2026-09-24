#pragma once
/* saber_engine::SpriteEffect — short-lived animated sprites (muzzle flashes, explosions, hit sparks). */
#include "gfx.h"
#include "character.h"

#define MAX_EFFECTS 128
typedef struct {
    bool alive;
    Sprite *spr; CBlock *cb;
    AnimDef anim; int frame; real t;
    real x, y, ox, oy, angle;
    int layer;
    const real *follow_x, *follow_y;   /* optional parent position */
    real fx0, fy0;
    bool flip;                          /* mirrored (set after spawning) */
} Effect;

typedef struct { Effect e[MAX_EFFECTS]; } Effects;

Effect *effects_spawn(Effects *fx, uint32_t sprite_id, int layer, const AnimDef *a, real x, real y, real ox, real oy, real angle);
void effects_update(Effects *fx, real dt);
void effects_draw(const Effects *fx, int layer, real cam_x, real cam_y);
