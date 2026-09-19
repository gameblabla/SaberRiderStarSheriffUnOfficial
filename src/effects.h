#pragma once
/* saber_engine::SpriteEffect — short-lived animated sprites (muzzle flashes, explosions, hit sparks). */
#include "gfx.h"
#include "character.h"

#define MAX_EFFECTS 128
typedef struct {
    bool alive;
    Sprite *spr; CBlock *cb;
    AnimDef anim; int frame; float t;
    float x, y, ox, oy, angle;
    int layer;
    const float *follow_x, *follow_y;   /* optional parent position */
    float fx0, fy0;
} Effect;

typedef struct { Effect e[MAX_EFFECTS]; } Effects;

Effect *effects_spawn(Effects *fx, uint32_t sprite_id, int layer, const AnimDef *a, float x, float y, float ox, float oy, float angle);
void effects_update(Effects *fx, float dt);
void effects_draw(const Effects *fx, int layer, float cam_x, float cam_y);
