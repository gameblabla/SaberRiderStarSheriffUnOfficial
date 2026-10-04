#pragma once
#include "pce_config.h"
/* What the Ramrod cockpit's HUD shows, filled by mech_frame (mech_pce.c) and drawn by hud6_draw (hud_pce.c, bank $7c). */
typedef struct {
    uint8_t armor,heat,hot,wave,left,lives,locked,banner,clock;
    uint16_t banner_t;
    uint8_t dot_on[3],dot_kind[3],dot_blink[3],threat[3],danger[3];
    int8_t dot_x[3],dot_y[3];
} Hud6;
extern Hud6 hud6;
void hud6_draw(void);
typedef struct {int16_t x;uint16_t distance,hp,clock;uint8_t variant,on;} Mech;
extern Mech mechs[3];
extern uint8_t visible[3],spawned,killed,aim,heat,gun_cd,punch_cd,overheated;
extern uint16_t wave_clock;
void mech_draw(void);

/* The space flight's HUD (space.c render_hud), filled by space_frame and drawn by hud7_draw (bank $7c). */
typedef struct {uint8_t hp,lives,power,bombs,items,boss_on,clock;uint16_t boss_hp;} Hud7;
extern Hud7 hud7;
void hud7_draw(void);
