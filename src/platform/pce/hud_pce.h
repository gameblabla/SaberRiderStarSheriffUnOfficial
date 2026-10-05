#pragma once
#include "pce_config.h"
/* The space flight's HUD (space.c render_hud), filled by space_frame and drawn by hud7_draw (bank $7c). */
typedef struct {uint8_t hp,lives,power,bombs,items,boss_on,clock;uint16_t boss_hp;} Hud7;
extern Hud7 hud7;
void hud7_draw(void);
