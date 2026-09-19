#pragma once
#include "character.h"
#include "input.h"
#include "bullets.h"
#include "effects.h"
typedef struct {
    Character ch;
    float fire_cooldown;     /* emitter +0xb14 */
    bool want_fire;
    bool locked;             /* +0xc08: controls disabled (cutscene/death) */
    int hp;
} Player;
void player_spawn(Player *p, uint32_t crhc_id, float x, float y);
void player_control(Player *p, const Input *in, float dt);   /* FUN_00422d10 input part */
/* FUN_0041ebd0: fire from the muzzle if the emitter cooldown allows; returns true if a shot was fired */
bool player_try_fire(Player *p, Bullets *bs, Effects *fx, int layer);
void player_frame_end(Player *p, float dt);
