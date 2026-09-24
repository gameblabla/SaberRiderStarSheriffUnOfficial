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

typedef struct { int32_t id, first, last, loop; real frame_time; uint32_t flags; } AnimDef;
typedef struct { real ox, oy, hw, hh; } HurtBox;

#define CHAR_CRHC_ANIMS 53          /* entries in the CRHC table */
#define CHAR_MAX_ANIMS 56           /* + slots for recreated heroes' extra animations (heroes.c) */
enum { WALK_AIM_ALL, WALK_AIM_DIAG, WALK_AIM_NONE };   /* torso overlays while walking: every aim (Fireball), only the up / down diagonals (April: her run torso already holds the gun level), never */

/* The animation / hurtbox / muzzle tables of a character type (a CRHC, with a recreated hero's patches), shared by
 * every character of that type: character_init looks it up (they used to be copied into each of the 64 enemy slots,
 * ~185 KB, which the Saturn's 1 MB of fast RAM can't spare). */
typedef struct CharDef {
    uint32_t crhc_id;
    AnimDef anims[CHAR_MAX_ANIMS];
    HurtBox hurt[CHAR_MAX_ANIMS];
    real muzzle[CHAR_MAX_ANIMS][2];
    uint32_t anim_flags[CHAR_MAX_ANIMS];
} CharDef;

typedef struct {
    /* definition (from CRHC) */
    const CharDef *def;
    uint32_t crhc_id, sprite_id;
    real box_ox, box_oy, box_hx, box_hy;   /* +0x24.. : physics box */
    real origin_x, origin_y;               /* +0x08 : sprite origin */
    real speed, slide_speed, slide_time, jump_vel, alert_time;   /* +0x10..0x20 */
    int hp_max;
    /* runtime */
    uint8_t state;
    uint32_t flags;
    uint8_t anim;
    uint8_t coll;
    uint8_t facing;      /* 0 left, 1 right */
    uint8_t aim;         /* AIM_* */
    real slide_t;
    real drop_target_y;
    real alert_t;
    real hit_t;
    bool walk_bob;              /* player walk cycle: torso follows the legs' 1 px bob (FUN_0041c530) */
    int8_t torso_bob[8];        /* px the hip drops on each run legs frame: every torso overlay rides it (Fireball's art: cells 2 and 5 sit 1 px lower) */
    uint8_t walk_aim_ov;        /* WALK_AIM_*: which aim / shoot torso overlays replace the run torso while walking */
    bool ov_sync;               /* a torso overlay with the legs' frame count and rate starts in phase with the legs (April: the run torso is the clip frames' upper half, so it must sit on its own legs frame) */
    int bored_anim[2];          /* L/R animation played once after idling for bored_time seconds (-1: none; April's stretch) */
    real bored_time, idle_t;
    real base_ox, base_oy;   /* +0x1c/+0x20 */
    real muzzle_x, muzzle_y; /* +0x24/+0x28 */
    /* animation playback */
    int frame; real anim_t;
    /* torso overlay (second sprite entity, player only) */
    uint8_t overlay; int ov_frame; real ov_t;
    Body body;
    CBlock *cb;
    Sprite *spr;          /* when the character's graphic is a .spr strip instead of a cblock */
} Character;

bool character_init(Character *c, uint32_t crhc_id, bool enemy);
/* a character's hurtbox for its current animation */
static inline const HurtBox *character_hurt(const Character *c) { return &c->def->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0]; }
void character_reset(Character *c, bool enemy);
void character_sync_ground(Character *c);                 /* FUN_0041ba90 */
void character_resolve(Character *c, real dt);           /* FUN_0041bc40: state -> anim + vx */
void character_set_anim(Character *c, int anim);          /* FUN_0041b710 */
void character_set_overlay(Character *c, int anim);       /* FUN_0041b570 */
void player_resolve(Character *c, real dt);              /* FUN_0041c530 */
void character_animate(Character *c, real dt);           /* SpriteAnimation update */
void character_draw(const Character *c, real cam_x, real cam_y);
/* input helpers (FUN_0041d740..) */
void character_move_left(Character *c, int vdir);
void character_move_right(Character *c, int vdir);
void character_down(Character *c);
void character_aim(Character *c, int aim);
void character_idle_aim(Character *c);
void character_aim_stand(Character *c);
void character_jump(Character *c);
bool character_request_shoot(Character *c);
