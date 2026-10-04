#pragma once
#include "pce_config.h"
bool floor_init(void);
void floor_update(uint16_t x,uint16_t y,uint8_t heading,uint8_t phase);
void floor_present(void);
/* the camera of the floor last completed (what the next frame shows): the sprites are projected from it */
extern uint16_t floor_shown_x,floor_shown_y;
extern uint8_t floor_shown_heading;
