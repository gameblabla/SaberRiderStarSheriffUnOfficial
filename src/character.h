#pragma once
/* Shared player/enemy character state (the 0xC10-byte struct initialised by FUN_0041b920). */
#include <stdint.h>
#include <stdbool.h>
#include "physics.h"
#include "gfx.h"

enum {  /* Character::state */
    CS_IDLE = 0, CS_WALK = 1, CS_AIR = 2, CS_JUMP = 3, CS_DROP = 4, CS_FALL = 5,
    CS_CROUCH = 6, CS_SLIDE = 7, CS_AIM = 8, CS_DEAD = 9
};
enum {  /* Character::flags */
    CF_IN_JUMP = 1, CF_DROPPING = 2, CF_CROUCH = 4, CF_DROP_REQ = 8, CF_SHOOT = 0x10,
    CF_ON_ONEWAY = 0x20, CF_HIT = 0x40, CF_DEAD = 0x80, CF_SPAWN_FALL = 0x100, CF_HIT_ALT = 0x200
};
enum { AIM_L = 0, AIM_UL, AIM_U, AIM_UR, AIM_R, AIM_DR, AIM_D, AIM_DL };

typedef struct { int32_t id, first, last, loop; float frame_time; uint32_t flags; } AnimDef;
typedef struct { float ox, oy, hw, hh; } HurtBox;

#define CHAR_CRHC_ANIMS 53          /* entries in the CRHC table */
#define CHAR_MAX_ANIMS 56           /* + slots for recreated heroes' extra animations (heroes.c) */

typedef struct {
    /* definition (from CRHC) */
    uint32_t crhc_id, sprite_id;
    float box_ox, box_oy, box_hx, box_hy;   /* +0x24.. : physics box */
    float origin_x, origin_y;               /* +0x08 : sprite origin */
    float speed, slide_speed, slide_time, jump_vel, alert_time;   /* +0x10..0x20 */
    AnimDef anims[CHAR_MAX_ANIMS];
    HurtBox hurt[CHAR_MAX_ANIMS];
    float muzzle[CHAR_MAX_ANIMS][2];
    uint32_t anim_flags[CHAR_MAX_ANIMS];
    int hp_max;
    /* runtime */
    uint8_t state;
    uint32_t flags;
    uint8_t anim;
    uint8_t coll;
    uint8_t facing;      /* 0 left, 1 right */
    uint8_t aim;         /* AIM_* */
    float slide_t;
    float drop_target_y;
    float alert_t;
    float hit_t;
    bool walk_bob;              /* player walk cycle: torso follows the legs' 1 px bob (FUN_0041c530) */
    bool torso_bob;             /* the sheet's run legs sit 1 px lower on cells 2 and 5 (Fireball's art) */
    int bored_anim[2];          /* L/R animation played once after idling for bored_time seconds (-1: none; April's stretch) */
    float bored_time, idle_t;
    float base_ox, base_oy;   /* +0x1c/+0x20 */
    float muzzle_x, muzzle_y; /* +0x24/+0x28 */
    /* animation playback */
    int frame; float anim_t;
    /* torso overlay (second sprite entity, player only) */
    uint8_t overlay; int ov_frame; float ov_t;
    Body body;
    CBlock *cb;
    Sprite *spr;          /* when the character's graphic is a .spr strip instead of a cblock */
} Character;

bool character_init(Character *c, uint32_t crhc_id, bool enemy);
void character_reset(Character *c, bool enemy);
void character_sync_ground(Character *c);                 /* FUN_0041ba90 */
void character_resolve(Character *c, float dt);           /* FUN_0041bc40: state -> anim + vx */
void character_set_anim(Character *c, int anim);          /* FUN_0041b710 */
void character_set_overlay(Character *c, int anim);       /* FUN_0041b570 */
void player_resolve(Character *c, float dt);              /* FUN_0041c530 */
void character_animate(Character *c, float dt);           /* SpriteAnimation update */
void character_draw(const Character *c, float cam_x, float cam_y);
/* input helpers (FUN_0041d740..) */
void character_move_left(Character *c, int vdir);
void character_move_right(Character *c, int vdir);
void character_down(Character *c);
void character_aim(Character *c, int aim);
void character_idle_aim(Character *c);
void character_aim_stand(Character *c);
void character_jump(Character *c);
bool character_request_shoot(Character *c);
