#pragma once
#include "play_pce.h"
#include "video_pce.h"
typedef struct { int16_t x,y,vx,vy; uint8_t fx,fy,coll,ground; } Body;
typedef struct { int16_t x,y,vx,vy; uint8_t active,enemy; } Shot;
typedef struct { Body b; uint8_t active,type,hp,timer,flip,dead,anim; } Actor;

extern Body player;
extern Actor actors[8];
extern Shot shots[24];
extern const PceScene *play_scene;
extern uint16_t camera,frame;
extern uint8_t hero,facing,safe_timer;
extern int16_t safe_x,safe_y;
extern uint8_t slide_time;
extern uint8_t cut_phase;   /* story camera pan around a level dialogue (combat_pce.c); 0 = none */
extern int16_t probe_x,probe_y;extern uint8_t probe_left;
void spawn_clear(void);
void actor_kill(Actor *a);
