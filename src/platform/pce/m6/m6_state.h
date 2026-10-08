#pragma once
#include "pce_config.h"
/* Ramrod's arena (stage 6): a sprite scaler in the manner of Space Harrier on the source's cast (ramrod.c): everything is kept in polar form round
 * Ramrod - a bearing in dots at the source's focal length (1344 to the turn, so a dot is a screen dot) and a distance in units - and drawn as a
 * sprite at the size its distance asks for (mech_draw_pce.c); Ramrod's own walking, turning and side-stepping move the bearings and distances.
 * Bearings are in 1/16 dot, distances in 1/4 unit. The state lives in the platform stages' trigger cache (staging memory the arena does not use). */
enum {S_OFF,S_ENTER,S_APPROACH,S_CIRCLE,S_AIM,S_FIRE,S_CHARGE,S_WINDUP,S_PUNCH,S_STAGGER,S_DYING};
typedef struct {
    int16_t ang,hp,lat0;uint16_t dist,fire_t,pref;
    uint8_t st,variant,st_t,flash,volley,dying,expl_t,anim;int8_t strafe,kick;
    uint8_t calm;   /* steps before it may swing at Ramrod again (a mech that has just punched backs off instead of punching on) */
} Mech6;
typedef struct {int16_t ang,z,vang,vd,vz,lat0;uint16_t dist;uint8_t life,enemy,dmg;} Shot6;   /* lat0: Ramrod's side-stepping when a plasma ball was fired */
typedef struct {int16_t ang,z;uint16_t dist;uint8_t t,dur,size;} Fx6;
typedef struct {int16_t ang;uint16_t dist;uint8_t kind;} Prop6;
typedef struct {
    Mech6 mech[3];Shot6 shot[16];Fx6 fx[10];Prop6 prop[14];
    int32_t cam;                         /* the sky's scroll in 1/16 dot, unwrapped */
    int16_t aim;                         /* the bearing Ramrod faces, 0..21503 (1344 dots of 1/16) */
    int16_t speed,strafe_v,turn_v,lat;   /* walking and side-stepping in 1/8 unit a step, turning in 1/16 dot a step; lat: the side-stepping so far, in 1/8 unit */
    uint16_t wave_t,rng;
    uint8_t boss_music,dead_t,cur,sp_w,sp_i,spawned,killed,heat,gun_cd,punch_cd,overheated,gun_idle,hurt,shake,flash_white,flash_red,gun_side,lock,fire_hold,punch_hold,banner,msg,msg_t,left;
    int8_t punch_t,punch_side;uint8_t punch_hit;
    int16_t par[4],along;                 /* the floor (m6_c.c m6_floor): the strafing's shift of each of four depth bands (1/16 dot, wrapping at the picture's 4096), and the way walked (1/128 of a texture line, wrapping at 96 lines) */
    uint16_t floor_x,floor_y;            /* the scroll the picture has chosen (m6_d.c) */
    int16_t lock_x,lock_y,lock_hp;       /* the locked mech's armour bar (the picture's m6_d.c, drawn by m6_hud) */
    int16_t blit_x,blit_y;uint16_t blit_key;uint8_t blit_kind,blit_flip,blit_ok;   /* the picture's call to the world's image (m6_d.c blit) */
    uint8_t threat[3];                   /* set by the picture (m6_d.c) for the HUD (m6_a.c): a mech out of view (1 left, 2 right) and bit 4 when it is about to fire or swing */
    uint32_t vram_address;uint16_t vram_word,vram_size; /* pattern upload handoff from the world/player image to the picture image */
    uint16_t bg_key[2];uint8_t bg_cache_valid[2],bg_map_valid[2];
    uint8_t bg_min_x[2],bg_max_x[2],bg_min_y[2],bg_max_y[2];
    uint8_t bg_actor_drawn,bg_palette_variant,bg_palette_valid;
} Arena6;
extern uint8_t trigger_cache[];
#define a6 (*(Arena6*)trigger_cache)
#define ARC 21504                      /* the turn in 1/16 dot */
/* Ramrod's arena is four code images (tools/pce/build_assets.py, Makefile.pce m6 rules, src/platform/pce/m6/): read from the stage's archive into the
 * four code banks the other stages use for the race and the CD buffer ($76, $77, $79, $7a, which loader_archive puts back after every load), linked
 * with the application into virtual banks 128-131 at $6000 (link.ld; m6_image.py cuts them out). They share the state above and call each other with
 * overlay_call. */
void m6_floor(void),m6_player(void),m6_step(void),m6_spawn(void),m6_start(void),m6_frame(void),m6_draw(void),m6_hud(void),m6_msgs(void),m6_blit(void);
void m6_vram_body(void);
void pce_sgx_arena_bg_draw_body(void);
#define M6A_BANK 0x76   /* the player: walking, turning, guns and fists */
#define M6B_BANK 0x77   /* the Renegades: their steps and plasma, their spawning */
#define M6C_BANK 0x79   /* the world: the clock, waves, shots, the lock, bursts */
#define M6D_BANK 0x7a   /* the picture */
