#pragma once
#include "pce_config.h"
void play_init(uint8_t stage, uint8_t hero);
void play_tick(uint8_t keys, uint8_t pressed);
void play_draw(void);
extern uint8_t pce_panel_restore;   /* BG row of a closed dialogue panel to restore on the next play_draw (0 = none) */
