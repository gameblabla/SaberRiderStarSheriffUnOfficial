#pragma once
#include "pce_config.h"
/* Arguments live in fixed console RAM while MPR3 changes. The trampoline,
 * stack, IRQs and every return into it remain mapped in resident bank $68. */
typedef struct {
    uint16_t x,y;
    uint8_t stage,hero,keys,pressed,elapsed,heading,phase,ok;
} PceControl;
extern PceControl pce_control;
void overlay_call(uint8_t bank,void (*method)(void));
void flow_main(void);
void play_start(void);
void encounter_init(void);
void actors_draw(void);
void shots_draw_pass(void);
void spawn_room(void);
void encounters(void);
void world_update(void);
void herd_spawn(void);
extern int16_t herd_y;
extern uint16_t herd_lead;
void herd_draw(void);
extern uint8_t herd_on,herd_locked,herd_pending,herd_flee;   /* herd_flee: the first stampede of level 1 (the enemies run off to the left) */
extern int16_t herd_next;
void herd_feed(void);
void play_frame(void);
void play_present(void);
void race_start(void);
void race_frame(void);
/* Ramrod's arena: code images loaded with the stage (m6/m6_state.h); the world's image is in bank $79 */
void m6_start(void),m6_frame(void);
#define M6_START m6_start
#define M6_FRAME m6_frame
void m6_load(void);
void space_start(void);
void space_frame(void);
extern uint8_t ui_lift_level;
void ui_lift(void);
