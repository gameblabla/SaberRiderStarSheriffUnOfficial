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
void encounters(void);
void play_frame(void);
void play_present(void);
void race_start(void);
void race_frame(void);
void mech_start(void);
void mech_frame(void);
void space_start(void);
void space_frame(void);
void floor_start(void);
void floor_draw(void);
