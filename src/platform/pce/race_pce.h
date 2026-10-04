#pragma once
#include "pce_config.h"
/* The Grand Prix and the pursuit: simulation in banks $79 (race_pce.c: the car, the field, the phases) and $7a
 * (race_foes_pce.c: shots, mines, the leader and his escort), sprites and HUD in bank $7c (race_draw_pce.c). */
typedef struct {uint16_t s;int8_t lap;int16_t lat,lat_t,speed;uint16_t max;uint8_t kind,hornet,hp,knock,t,t2;int16_t x,y,gap;} Rival;
typedef struct __attribute__((packed)) {uint16_t x,y;uint8_t heading;} TrackPoint;
typedef struct {int16_t x,y;uint8_t t;} Mine;
typedef struct {int16_t x,y,vx,vy;uint8_t t,own;} Bolt;
/* The leader and his escort: positions in Q8 (x, y are the whole units), anim the phase of the weave. */
typedef struct {int32_t xq,yq;int16_t x,y,speed;uint16_t anim,since;uint8_t state,hp,hp_max,knock,boost,t,t2,t3;} Leader;
enum {P_COUNT,P_RACE,P_FINISH,P_PURSUIT,P_BOSS,P_VICTORY};
extern TrackPoint track[256];
extern Rival rv[7];
extern Mine mines[6];
extern Bolt race_bolts[6];
extern Leader boss,escort[2];
extern uint16_t px,py,hd,cam_hd,phase_t,race_time,gap_dist,race_rng,ps;
extern int16_t speed,tilt;
extern uint8_t rphase,car_hp,car_max,boost,boost_on,hurt,shake,finish_rank,spin,ram_cd;
extern int8_t lapp,cam_c,cam_s;
/* arguments of the calls across banks */
extern int16_t arg_x,arg_y,arg_dist,arg_radius;extern uint8_t arg_damage,arg_life;extern int16_t arg_speed;
void race_start(void);
void race_frame(void);
void race_draw(void);
void hurt_call(void);          /* arg_damage */
void bump_call(void);          /* arg_x, arg_y, arg_dist, arg_radius (bank $6d with the field) */
void field_start_call(void),field_update_call(void),field_standings_call(void),field_point_call(void);
void foes_shots(void),foes_leader(void),foes_escorts(void),foes_leader_start(void),foes_spawn_escort(void);
void foes_drop_mine(void);     /* arg_x, arg_y, arg_life */
void foes_aimed_bolt(void);    /* arg_x, arg_y, arg_speed, arg_life */
