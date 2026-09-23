#pragma once
#include "character.h"
#include "input.h"
#include "bullets.h"
#include "effects.h"
typedef struct {
    Character ch;
    float fire_cooldown;     /* emitter +0xb14 */
    bool want_fire;
    float dead_t;
    float safe_x, safe_y, respawn_x, respawn_y;
    int lives;
    bool game_over;
    bool locked;             /* +0xc08: controls disabled (cutscene/death) */
    bool fire_hold;          /* set through a dialogue: no shot until the shoot button (used to page it) is let go */
    bool quiet;              /* no jump grunt (stage 5's Dark April runs on player_control too) */
    int hp, max_hp;          /* hearts: 4 / 2 / 0 by difficulty (DAT_00ac9be0[1]) */
} Player;
void player_spawn(Player *p, uint32_t crhc_id, float x, float y);
void player_control(Player *p, const Input *in, float dt);   /* FUN_00422d10 input part */
/* FUN_0041ebd0: fire from the muzzle if the emitter cooldown allows; returns true if a shot was fired */
bool player_try_fire(Player *p, Bullets *bs, Effects *fx, int layer);
void player_frame_end(Player *p, float dt);
/* death timer + respawn (FUN_00422d10 state-9 branch); returns true when a respawn happened */
bool player_death_update(Player *p, float dt, float level_h, float *cam_x, int sw);
