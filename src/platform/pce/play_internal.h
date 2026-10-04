#pragma once
#include "play_pce.h"
#include "video_pce.h"
typedef struct { int16_t x,y,vx,vy; uint8_t fx,fy,coll,ground; } Body;
/* Shots move in Q8 (px per step) with a fractional carry, so the source's 166 / 200 / 500 px/s bullets and the
 * kneelers' lobbed grenades keep their speeds. enemy: 0 hero shot, 1 enemy bullet, 2 grenade (t = steps in flight). */
typedef struct { int16_t x,y,vx,vy; uint8_t active,enemy,fx,fy,t; } Shot;
/* aim: the source's 8-way aim of snipers / kneelers (0 L, 1 UL, 2 U, 3 UR, 4 R, 5 DR, 6 D, 7 DL). mode: bit0 the
 * grunt's one shot is still unspent, bit1 a kneeler's throw is winding up, bit2 shield burning. timer: the grunt's
 * firing-stand step, the sniper's aim-settle clock, the kneeler's throw clock, the shield's rifle clock. */
typedef struct { Body b; uint8_t active,type,hp,timer,flip,dead,anim,aim,mode; } Actor;
_Static_assert(sizeof(Actor)==21,"herd_prepare.S assumes 21-byte actors");
#define NSHOTS 16

extern Body player;
extern Actor actors[8];
extern Shot shots[NSHOTS];
extern const PceScene *play_scene;
extern uint16_t camera,frame;
extern uint8_t hero,facing,safe_timer;
extern int16_t safe_x,safe_y;
extern uint8_t slide_time;
extern uint16_t hero_sprite;   /* the sprite id play_draw last chose for the hero */
extern uint8_t cut_phase;   /* story camera pan around a level dialogue (combat_pce.c); 0 = none */
extern int16_t probe_x,probe_y;extern uint8_t probe_left;
void spawn_clear(void);
void actor_kill(Actor *a);

void shots_draw(void);
