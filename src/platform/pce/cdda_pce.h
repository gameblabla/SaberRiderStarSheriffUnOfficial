#pragma once
#include "pce_config.h"

extern volatile uint8_t pce_music_status;
void cdda_start(uint8_t track,bool repeat);
void cdda_stop(void);
void cdda_tick(void);
bool cdda_busy(void);
